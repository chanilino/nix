#include "build.hh"
#include "misc.hh"


Derivation derivationFromPath(const Path & drvPath)
{
    assertStorePath(drvPath);
    ensurePath(drvPath);
    ATerm t = ATreadFromNamedFile(drvPath.c_str());
    if (!t) throw Error(format("cannot read aterm from `%1%'") % drvPath);
    return parseDerivation(t);
}


void computeFSClosure(const Path & storePath,
    PathSet & paths, bool flipDirection)
{
    if (paths.find(storePath) != paths.end()) return;
    paths.insert(storePath);

    PathSet references;
    if (flipDirection)
        queryReferers(noTxn, storePath, references);
    else
        queryReferences(noTxn, storePath, references);

    for (PathSet::iterator i = references.begin();
         i != references.end(); ++i)
        computeFSClosure(*i, paths, flipDirection);
}


OutputEqClass findOutputEqClass(const Derivation & drv, const string & id)
{
    DerivationOutputs::const_iterator i = drv.outputs.find(id);
    if (i == drv.outputs.end())
        throw Error(format("derivation has no output `%1%'") % id);
    return i->second.eqClass;
}


Path findTrustedEqClassMember(const OutputEqClass & eqClass,
    const TrustId & trustId)
{
    OutputEqMembers members;
    queryOutputEqMembers(noTxn, eqClass, members);

    for (OutputEqMembers::iterator j = members.begin(); j != members.end(); ++j)
        if (j->trustId == trustId || j->trustId == "root") return j->path;

    return "";
}


typedef map<OutputEqClass, PathSet> ClassMap;
typedef map<OutputEqClass, Path> FinalClassMap;


static void findBestRewrite(const ClassMap::const_iterator & pos,
    const ClassMap::const_iterator & end,
    const PathSet & selection, const PathSet & unselection,
    unsigned int & bestCost, PathSet & bestSelection)
{
    if (pos != end) {
        for (PathSet::iterator i = pos->second.begin();
             i != pos->second.end(); ++i)
        {
            PathSet selection2(selection);
            selection2.insert(*i);
            
            PathSet unselection2(unselection);
            for (PathSet::iterator j = pos->second.begin();
                 j != pos->second.end(); ++j)
                if (i != j) unselection2.insert(*j);
            
            ClassMap::const_iterator j = pos; ++j;
            findBestRewrite(j, end, selection2, unselection2,
                bestCost, bestSelection);
        }
        return;
    }

    //    printMsg(lvlError, format("selection %1%") % showPaths(selection));
    
    PathSet badPaths;
    for (PathSet::iterator i = selection.begin();
         i != selection.end(); ++i)
    {
        PathSet closure;
        computeFSClosure(*i, closure); 
        for (PathSet::iterator j = closure.begin();
             j != closure.end(); ++j)
            if (unselection.find(*j) != unselection.end())
                badPaths.insert(*i);
    }
    
    printMsg(lvlError, format("cost %1% %2%") % badPaths.size() % showPaths(badPaths));

    if (badPaths.size() < bestCost) {
        bestCost = badPaths.size();
        bestSelection = selection;
    }
}


static Path maybeRewrite(const Path & path, const PathSet & selection,
    const FinalClassMap & finalClassMap)
{
    assert(selection.find(path) != selection.end());
    
    PathSet references;
    queryReferences(noTxn, path, references);

    HashRewrites rewrites;
    
    bool okay = true;
    for (PathSet::iterator i = references.begin(); i != references.end(); ++i) {
        if (*i == path) continue; /* ignore self-references */
        if (selection.find(*i) == selection.end()) {
            OutputEqClasses classes;
            queryOutputEqClasses(noTxn, *i, classes);
            
            if (classes.size() > 0) /* !!! hacky; ignore sources; they
                                       are not in any eq class */
            {
                printMsg(lvlError, format("in `%1%': missing `%2%'") % path % *i);
                okay = false;

                FinalClassMap::const_iterator j = finalClassMap.find(*(classes.begin()));
                assert(j != finalClassMap.end());

                printMsg(lvlError, format("replacing with `%1%'") % j->second);
                
                Path newPath = maybeRewrite(j->second, selection, finalClassMap);
                if (*i != newPath)
                    rewrites[hashPartOf(*i)] = hashPartOf(newPath);
            }
        }
    }

    if (rewrites.size() == 0) return path;

    printMsg(lvlError, format("rewriting `%1%'") % path);

    Path newPath = addToStore(path,
        hashPartOf(path), namePartOf(path),
        references, rewrites);

    printMsg(lvlError, format("rewrote `%1%' to `%2%'") % path % newPath);

    return newPath;
}


PathSet consolidatePaths(const PathSet & paths, bool checkOnly)
{
    printMsg(lvlError, format("consolidating"));
    
    ClassMap classMap;
    
    for (PathSet::const_iterator i = paths.begin(); i != paths.end(); ++i) {
        OutputEqClasses classes;
        queryOutputEqClasses(noTxn, *i, classes);

        /* !!! deal with sources */
        
        for (OutputEqClasses::iterator j = classes.begin(); j != classes.end(); ++j) {
            classMap[*j].insert(*i);
        }
    }

    bool conflict = false;
    for (ClassMap::iterator i = classMap.begin(); i != classMap.end(); ++i)
        if (i->second.size() >= 2) {
            printMsg(lvlError, format("conflict in eq class `%1%'") % i->first);
            conflict = true;
        }

    if (!conflict) return paths;
    
    assert(!checkOnly);
    
    /* !!! exponential-time algorithm! */
    const unsigned int infinity = 1000000;
    unsigned int bestCost = infinity;
    PathSet bestSelection;
    findBestRewrite(classMap.begin(), classMap.end(),
        PathSet(), PathSet(), bestCost, bestSelection);

    assert(bestCost != infinity);

    printMsg(lvlError, format("cheapest selection %1% %2%")
        % bestCost % showPaths(bestSelection));

    FinalClassMap finalClassMap;
    for (ClassMap::iterator i = classMap.begin(); i != classMap.end(); ++i)
        for (PathSet::const_iterator j = i->second.begin(); j != i->second.end(); ++j)
            if (bestSelection.find(*j) != bestSelection.end())
                finalClassMap[i->first] = *j;

    PathSet newPaths;
    for (PathSet::iterator i = bestSelection.begin();
         i != bestSelection.end(); ++i)
        newPaths.insert(maybeRewrite(*i, bestSelection, finalClassMap));
    
    return newPaths;
}
