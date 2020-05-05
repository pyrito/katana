/*
 * This file belongs to the Galois project, a C++ library for exploiting
 * parallelism. The code is being released under the terms of the 3-Clause BSD
 * License (a copy is located in LICENSE.txt at the top-level directory).
 *
 * Copyright (C) 2018, The University of Texas at Austin. All rights reserved.
 * UNIVERSITY EXPRESSLY DISCLAIMS ANY AND ALL WARRANTIES CONCERNING THIS
 * SOFTWARE AND DOCUMENTATION, INCLUDING ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR ANY PARTICULAR PURPOSE, NON-INFRINGEMENT AND WARRANTIES OF
 * PERFORMANCE, AND ANY WARRANTY THAT MIGHT OTHERWISE ARISE FROM COURSE OF
 * DEALING OR USAGE OF TRADE.  NO WARRANTY IS EITHER EXPRESS OR IMPLIED WITH
 * RESPECT TO THE USE OF THE SOFTWARE OR DOCUMENTATION. Under no circumstances
 * shall University be liable for incidental, special, indirect, direct or
 * consequential damages or loss of profits, interruption of business, or
 * related expenses which may arise from use of Software or Documentation,
 * including but not limited to those resulting from defects in Software and/or
 * Documentation, or loss or inaccuracy of data of any kind.
 */

#include "galois/Galois.h"
#include "galois/Bag.h"
#include "galois/Timer.h"
#include "galois/Timer.h"
#include "galois/graphs/Graph.h"
#include "galois/graphs/TypeTraits.h"
#include "llvm/Support/CommandLine.h"
#include "Lonestar/BoilerPlate.h"
#include "galois/Reduction.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <atomic>
#include <functional>
#include <type_traits>
#include <set>

#include <chrono>
#include <random>

namespace cll = llvm::cl;

static const char* name = "Subgraph Isomorphism";
static const char* desc =
    "Computes up to k subgraph isomorphism on data graph for query graph";
static const char* url = "subgraph_isomorphism";

enum Algo { ullmann, vf2 };

static cll::opt<unsigned int>
    kFound("kFound", cll::desc("stop when k instances found"), cll::init(10));
static cll::opt<bool> undirected("undirected",
                                 cll::desc("undirected data and query graphs"),
                                 cll::init(false));

static cll::opt<std::string> graphD("graphD", cll::desc("<data graph file>"));
static cll::opt<std::string> graphQ("graphQ", cll::desc("<query graph file>"));

static cll::opt<unsigned int> numLabels("numLabels", cll::desc("# labels"),
                                        cll::init(2));

static cll::opt<bool>
    rndSeedQByTime("rndSeedQByTime",
                   cll::desc("rndSeedQ generated by system time"),
                   cll::init(false));
static cll::opt<unsigned int> rndSeedQ("rndSeedQ", cll::desc("random seed Q"),
                                       cll::init(0));

static cll::opt<bool>
    rndSeedDByTime("rndSeedDByTime",
                   cll::desc("rndSeedD generated by system time"),
                   cll::init(false));
static cll::opt<unsigned int> rndSeedD("rndSeedD", cll::desc("random seed D"),
                                       cll::init(0));

static cll::opt<Algo>
    algo("algo", cll::desc("Choose an algorithm:"),
         cll::values(clEnumValN(Algo::ullmann, "ullmann", "Ullmann (default)"),
                     clEnumValN(Algo::vf2, "vf2", "VF2"), clEnumValEnd),
         cll::init(Algo::ullmann));

struct DNode {
  char label;
  unsigned int id;
};

typedef galois::graphs::LC_CSR_Graph<DNode, void>::template with_no_lockable<
    true>::type
    InnerDGraph; // graph with DNode nodes and typeless edges without locks
typedef galois::graphs::LC_InOut_Graph<InnerDGraph>
    DGraph; // for incoming neighbors
typedef DGraph::GraphNode DGNode;

struct QNode {
  char label;
  unsigned int id;
  std::vector<DGNode> candidate;
};

typedef galois::graphs::LC_CSR_Graph<QNode, void>::template with_no_lockable<
    true>::type InnerQGraph;
typedef galois::graphs::LC_InOut_Graph<InnerQGraph> QGraph;
typedef QGraph::GraphNode QGNode;

struct NodeMatch {
  QGNode nQ;
  DGNode nD;
  NodeMatch(const QGNode q, const DGNode d) : nQ(q), nD(d) {}
  NodeMatch() : nQ(), nD() {}
};

typedef std::vector<NodeMatch> Matching;
typedef galois::InsertBag<Matching> MatchingVector;

static std::minstd_rand0 generator;
static std::uniform_int_distribution<unsigned> distribution;

static std::atomic_uint currentlyFound;

template <typename Graph>
void initializeGraph(Graph& g, unsigned int seed) {
  typedef typename Graph::node_data_type Node;

  generator.seed(seed);

  unsigned int i = 0;
  for (auto n : g) {
    Node& data = g.getData(n);
    data.id    = i++;
    data.label = 'A' + distribution(generator) % numLabels;
  }
}

struct VF2Algo {
  std::string name() const { return "VF2"; }

  class FilterCandidatesInternal {
    DGraph& gD;
    QGraph& gQ;
    galois::GReduceLogicalOR& nodeEmpty;
    FilterCandidatesInternal(DGraph& d, QGraph& q,
                             galois::GReduceLogicalOR& lor)
        : gD(d), gQ(q), nodeEmpty(lor) {}

  public:
    void operator()(const QGNode nQ) const {
      auto& dQ = gQ.getData(nQ);

      for (auto nD : gD) {
        auto& dD = gD.getData(nD);
        if (dQ.label != dD.label)
          continue;

        // self loop for nQ but not for nD
        if (gQ.findEdgeSortedByDst(nQ, nQ) != gQ.edge_end(nQ) &&
            gD.findEdgeSortedByDst(nD, nD) == gD.edge_end(nD))
          continue;

        dQ.candidate.push_back(nD);
      }

      std::sort(dQ.candidate.begin(), dQ.candidate.end());
      assert(std::adjacent_find(dQ.candidate.begin(), dQ.candidate.end()) ==
             dQ.candidate.end()); // no duplicates

      nodeEmpty.update(dQ.candidate.empty());
    }

    // return true if at least one node has an empty set of candidates
    static bool go(DGraph& gD, QGraph& gQ) {
      galois::GReduceLogicalOR isSomeNodeEmpty;
      galois::do_all(gQ, FilterCandidatesInternal(gD, gQ, isSomeNodeEmpty),
                     galois::loopname("filter"), galois::steal());
      return isSomeNodeEmpty.reduce();
    }
  };

  struct SubgraphSearchInternal {
    DGraph& gD;
    QGraph& gQ;
    MatchingVector& report;
    SubgraphSearchInternal(DGraph& d, QGraph& q, MatchingVector& r)
        : gD(d), gQ(q), report(r) {}

    struct LocalState {
      template <typename T>
      using PerIterAlloc = typename galois::PerIterAllocTy::rebind<T>::other;

      // query state
      std::set<QGNode, std::less<QGNode>, PerIterAlloc<QGNode>> qFrontier;
      std::set<QGNode, std::less<QGNode>, PerIterAlloc<QGNode>> qMatched;

      // data state
      std::set<DGNode, std::less<DGNode>, PerIterAlloc<DGNode>> dFrontier;
      std::set<DGNode, std::less<DGNode>, PerIterAlloc<DGNode>> dMatched;

      LocalState(galois::PerIterAllocTy& a)
          : qFrontier(a), qMatched(a), dFrontier(a), dMatched(a) {}

      QGNode nextQueryNode(QGraph& gQ, Matching& matching) {
        if (qFrontier.size())
          return *(qFrontier.begin());
        else
          for (auto nQ : gQ) {
            bool isMatched = false;
            for (auto& mi : matching)
              if (nQ == mi.nQ) {
                isMatched = true;
                break;
              }
            if (!isMatched)
              return nQ;
          }

        // never reaches here. if so, abort.
        abort();
      }
    };

    // for counting occurences only. no space allocation is required.
    template <typename T>
    class counter : public std::iterator<std::output_iterator_tag, T> {
      T dummy;
      long int num;

    public:
      counter() : num(0) {}
      counter& operator++() {
        ++num;
        return *this;
      }
      counter operator++(int) {
        auto retval = *this;
        ++num;
        return retval;
      }
      T& operator*() { return dummy; }
      long int get() { return num; }
    };

    template <typename Graph, typename Set>
    long int countInNeighbors(Graph& g, typename Graph::GraphNode n,
                              Set& sMatched) {
      using Iter = typename Graph::in_edge_iterator;

      // lambda expression. captures Graph& for expression body.
      auto l = [&g](Iter i) { return g.getInEdgeDst(i); };

      counter<typename Graph::GraphNode> count;

      std::set_difference(
          // galois::NoDerefIterator lets dereference return the wrapped
          // iterator itself boost::make_transform_iterator gives an iterator,
          // which is dereferenced to func(in_iter)
          boost::make_transform_iterator(
              galois::NoDerefIterator<Iter>(g.in_edge_begin(n)), l),
          boost::make_transform_iterator(
              galois::NoDerefIterator<Iter>(g.in_edge_end(n)), l),
          sMatched.begin(), sMatched.end(), count);
      return count.get();
    }

    template <typename Graph, typename Set>
    long int countNeighbors(Graph& g, typename Graph::GraphNode n,
                            Set& sMatched) {
      using Iter = typename Graph::edge_iterator;

      auto l = [&g](Iter i) { return g.getEdgeDst(i); };
      counter<typename Graph::GraphNode> count;

      std::set_difference(
          boost::make_transform_iterator(
              galois::NoDerefIterator<Iter>(g.edge_begin(n)), l),
          boost::make_transform_iterator(
              galois::NoDerefIterator<Iter>(g.edge_end(n)), l),
          sMatched.begin(), sMatched.end(), count);
      return count.get();
    }

    std::vector<DGNode, LocalState::PerIterAlloc<DGNode>>
    refineCandidates(DGraph& gD, QGraph& gQ, QGNode nQuery,
                     galois::PerIterAllocTy& alloc, LocalState& state) {
      std::vector<DGNode, LocalState::PerIterAlloc<DGNode>> refined(alloc);
      auto numNghQ = std::distance(gQ.edge_begin(nQuery), gQ.edge_end(nQuery));
      long int numUnmatchedNghQ = countNeighbors(gQ, nQuery, state.qMatched);

      long int numInNghQ = 0, numUnmatchedInNghQ = 0;
      if (!undirected) {
        numInNghQ =
            std::distance(gQ.in_edge_begin(nQuery), gQ.in_edge_end(nQuery));
        numUnmatchedInNghQ = countInNeighbors(gQ, nQuery, state.qMatched);
      }

      // consider all nodes in data frontier
      auto& dQ = gQ.getData(nQuery);
      for (auto ii : state.dFrontier) {
        // not a candidate for nQuery
        if (!std::binary_search(dQ.candidate.begin(), dQ.candidate.end(), ii))
          continue;

        auto numNghD = std::distance(gD.edge_begin(ii), gD.edge_end(ii));
        if (numNghD < numNghQ)
          continue;

        long int numUnmatchedNghD = countNeighbors(gD, ii, state.dMatched);
        if (numUnmatchedNghD < numUnmatchedNghQ)
          continue;

        if (undirected) {
          refined.push_back(ii);
          continue;
        }

        auto numInNghD =
            std::distance(gD.in_edge_begin(ii), gD.in_edge_end(ii));
        if (numInNghD < numInNghQ)
          continue;

        long int numUnmatchedInNghD = countInNeighbors(gD, ii, state.dMatched);
        if (numUnmatchedInNghD < numUnmatchedInNghQ)
          continue;

        refined.push_back(ii);
      }
      return refined;
    }

    bool isJoinable(DGraph& gD, QGraph& gQ, DGNode nD, QGNode nQ,
                    Matching& matching) {
      for (auto& nm : matching) {
        // nD is already matched
        if (nD == nm.nD)
          return false;

        // nQ => (nm.nQ) exists but not nD => (nm.nD)
        if (gQ.findEdgeSortedByDst(nQ, nm.nQ) != gQ.edge_end(nQ) &&
            gD.findEdgeSortedByDst(nD, nm.nD) == gD.edge_end(nD))
          return false;

        // (nm.nQ) => nQ exists but not (nm.nD) => nD
        // skip if both data and query graphs are directed
        if (!undirected)
          if (gQ.findEdgeSortedByDst(nm.nQ, nQ) != gQ.edge_end(nm.nQ) &&
              gD.findEdgeSortedByDst(nm.nD, nD) == gD.edge_end(nm.nD))
            return false;
      }

      return true;
    }

    template <typename StateSet, typename StepSet, typename Graph>
    void insertDstFrontierTracked(StateSet& sMatched, StateSet& sFrontier,
                                  StepSet& sAdd2F, Graph& g,
                                  typename Graph::GraphNode n) {
      for (auto e : g.edges(n)) {
        auto ngh = g.getEdgeDst(e);
        if (!sMatched.count(ngh) && sFrontier.insert(ngh).second)
          sAdd2F.push_back(ngh);
      }
    }

    template <typename StateSet, typename StepSet, typename Graph>
    void insertInDstFrontierTracked(StateSet& sMatched, StateSet& sFrontier,
                                    StepSet& sAdd2F, Graph& g,
                                    typename Graph::GraphNode n) {
      for (auto ie : g.in_edges(n)) {
        auto ngh = g.getInEdgeDst(ie);
        if (!sMatched.count(ngh) && sFrontier.insert(ngh).second)
          sAdd2F.push_back(ngh);
      }
    }

    void doSearch(LocalState& state, Matching& matching,
                  galois::PerIterAllocTy& alloc) {
      if (currentlyFound.load() >= kFound)
        return;

      if (matching.size() == gQ.size()) {
        report.push_back(matching);
        currentlyFound += 1;
        return;
      }

      auto nQ      = state.nextQueryNode(gQ, matching);
      auto refined = refineCandidates(gD, gQ, nQ, alloc, state);

      // update query state
      state.qMatched.insert(nQ);
      state.qFrontier.erase(nQ);

      std::vector<QGNode, LocalState::PerIterAlloc<QGNode>> qAdd2Frontier(
          alloc);
      insertDstFrontierTracked(state.qMatched, state.qFrontier, qAdd2Frontier,
                               gQ, nQ);
      if (!undirected)
        insertInDstFrontierTracked(state.qMatched, state.qFrontier,
                                   qAdd2Frontier, gQ, nQ);

      // search for all possible candidate data nodes
      for (auto r : refined) {
        if (!isJoinable(gD, gQ, r, nQ, matching))
          continue;

        // add (nQ, r) to matching
        matching.push_back(NodeMatch(nQ, r));

        // update data state
        state.dMatched.insert(r);
        state.dFrontier.erase(r);

        std::vector<DGNode, LocalState::PerIterAlloc<DGNode>> dAdd2Frontier(
            alloc);
        insertDstFrontierTracked(state.dMatched, state.dFrontier, dAdd2Frontier,
                                 gD, r);
        if (!undirected)
          insertInDstFrontierTracked(state.dMatched, state.dFrontier,
                                     dAdd2Frontier, gD, r);

        doSearch(state, matching, alloc);
        if (currentlyFound.load() >= kFound)
          return;

        // restore data state
        state.dMatched.erase(r);
        state.dFrontier.insert(r);
        for (auto i : dAdd2Frontier)
          state.dFrontier.erase(i);
        dAdd2Frontier.clear();

        // remove (nQ, r) from matching
        matching.pop_back();
      }

      // restore query state
      state.qMatched.erase(nQ);
      state.qFrontier.insert(nQ);
      for (auto i : qAdd2Frontier)
        state.qFrontier.erase(i);
    }

    template <typename Set, typename Graph>
    void insertDstFrontier(Set& sMatched, Set& sFrontier, Graph& g,
                           typename Graph::GraphNode n) {
      for (auto e : g.edges(n)) {
        auto ngh = g.getEdgeDst(e);
        if (!sMatched.count(ngh))
          sFrontier.insert(ngh);
      }
    }

    template <typename Set, typename Graph>
    void insertInDstFrontier(Set& sMatched, Set& sFrontier, Graph& g,
                             typename Graph::GraphNode n) {
      for (auto ie : g.in_edges(n)) {
        auto ngh = g.getInEdgeDst(ie);
        if (!sMatched.count(ngh))
          sFrontier.insert(ngh);
      }
    }

    // galois::for_each expects ctx
    void operator()(NodeMatch& seed, galois::UserContext<NodeMatch>& ctx) {
      LocalState state(ctx.getPerIterAlloc());

      auto nQ = seed.nQ;
      state.qMatched.insert(nQ);

      insertDstFrontier(state.qMatched, state.qFrontier, gQ, nQ);
      if (!undirected)
        insertInDstFrontier(state.qMatched, state.qFrontier, gQ, nQ);

      auto nD = seed.nD;
      state.dMatched.insert(nD);

      insertDstFrontier(state.dMatched, state.dFrontier, gD, nD);
      if (!undirected)
        insertInDstFrontier(state.dMatched, state.dFrontier, gD, nD);

      Matching matching{seed};
      doSearch(state, matching, ctx.getPerIterAlloc());

      if (currentlyFound.load() >= kFound)
        ctx.breakLoop();
    }
  };

public:
  // return true if at least one node has an empty set of candidates
  static bool filterCandidates(DGraph& gD, QGraph& gQ) {
    return FilterCandidatesInternal::go(gD, gQ);
  }

  static MatchingVector subgraphSearch(DGraph& gD, QGraph& gQ) {
    // parallelize the search for candidates of gQ.begin()
    galois::InsertBag<NodeMatch> works;
    auto nQ = *(gQ.begin());
    for (auto c : gQ.getData(nQ).candidate)
      works.push_back(NodeMatch(nQ, c));

    MatchingVector report;
    galois::for_each(works, SubgraphSearchInternal(gD, gQ, report),
                     galois::loopname("search_for_each"),
                     galois::no_conflicts(), galois::no_pushes(),
                     galois::parallel_break(), galois::per_iter_alloc());
    return report;
  }
};

struct UllmannAlgo {
  std::string name() const { return "Ullmann"; }

  struct FilterCandidatesInternal {
    DGraph& gD;
    QGraph& gQ;
    galois::GReduceLogicalOR& nodeEmpty;
    FilterCandidatesInternal(DGraph& d, QGraph& q,
                             galois::GReduceLogicalOR& lor)
        : gD(d), gQ(q), nodeEmpty(lor) {}

    void operator()(const QGNode nQ) const {
      auto& dQ = gQ.getData(nQ);

      for (auto nD : gD) {
        auto& dD = gD.getData(nD);

        if (dQ.label != dD.label)
          continue;

        // self loop for nQ but not for nD
        if (gQ.findEdgeSortedByDst(nQ, nQ) != gQ.edge_end(nQ) &&
            gD.findEdgeSortedByDst(nD, nD) == gD.edge_end(nD))
          continue;

        dQ.candidate.push_back(nD);
      }

      nodeEmpty.update(dQ.candidate.empty());
    }

    // return true if at least one node has an empty set of candidates
    static bool go(DGraph& gD, QGraph& gQ) {
      galois::GReduceLogicalOR isSomeNodeEmpty;
      galois::do_all(gQ, FilterCandidatesInternal(gD, gQ, isSomeNodeEmpty),
                     galois::loopname("filter"), galois::steal());
      return isSomeNodeEmpty.reduce();
    }
  };

  struct SubgraphSearchInternal {
    DGraph& gD;
    QGraph& gQ;
    MatchingVector& report;
    SubgraphSearchInternal(DGraph& d, QGraph& q, MatchingVector& r)
        : gD(d), gQ(q), report(r) {}

    QGNode nextQueryNode(QGraph& gQ, Matching& matching) {
      auto qi = gQ.begin();
      std::advance(qi, matching.size());
      return *qi;
    }

    std::vector<DGNode> refineCandidates(DGraph& gD, QGraph& gQ, QGNode nQuery,
                                         Matching& matching) {
      std::vector<DGNode> refined;
      auto& dQ     = gQ.getData(nQuery);
      auto numNghQ = std::distance(gQ.edge_begin(nQuery), gQ.edge_end(nQuery));
      auto numInNghQ =
          std::distance(gQ.in_edge_begin(nQuery), gQ.in_edge_end(nQuery));

      for (auto c : dQ.candidate) {
        auto numNghD   = std::distance(gD.edge_begin(c), gD.edge_end(c));
        auto numInNghD = std::distance(gD.in_edge_begin(c), gD.in_edge_end(c));
        if (numNghD >= numNghQ && numInNghD >= numInNghQ)
          refined.push_back(c);
      }

      return refined;
    }

    bool isJoinable(DGraph& gD, QGraph& gQ, DGNode nD, QGNode nQ,
                    Matching& matching) {
      for (auto& nm : matching) {
        // nD is already matched
        if (nD == nm.nD)
          return false;

        // nQ => (nm.nQ) exists but not nD => (nm.nD)
        if (gQ.findEdgeSortedByDst(nQ, nm.nQ) != gQ.edge_end(nQ) &&
            gD.findEdgeSortedByDst(nD, nm.nD) == gD.edge_end(nD))
          return false;

        // (nm.nQ) => nQ exists but not (nm.nD) => nD
        // skip if both data and query graphs are directed
        if (!undirected)
          if (gQ.findEdgeSortedByDst(nm.nQ, nQ) != gQ.edge_end(nm.nQ) &&
              gD.findEdgeSortedByDst(nm.nD, nD) == gD.edge_end(nm.nD))
            return false;
      }

      return true;
    }

    void doSearch(Matching& matching) {
      if (currentlyFound.load() >= kFound)
        return;

      if (matching.size() == gQ.size()) {
        report.push_back(matching);
        currentlyFound += 1;
        return;
      }

      auto nQ      = nextQueryNode(gQ, matching);
      auto refined = refineCandidates(gD, gQ, nQ, matching);

      for (auto r : refined) {
        if (!isJoinable(gD, gQ, r, nQ, matching))
          continue;

        // add (nQ, r) to matching
        matching.push_back(NodeMatch(nQ, r));

        doSearch(matching);
        if (currentlyFound.load() >= kFound)
          return;

        // remove (nQ, r) from matching
        matching.pop_back();
      }
    }

    // galois::for_each expects ctx
    void operator()(NodeMatch& seed, galois::UserContext<NodeMatch>& ctx) {
      Matching matching{seed};
      doSearch(matching);
      if (currentlyFound.load() >= kFound)
        ctx.breakLoop();
    }
  };

public:
  // return true if at least one node has an empty set of candidates
  static bool filterCandidates(DGraph& gD, QGraph& gQ) {
    return FilterCandidatesInternal::go(gD, gQ);
  }

  static MatchingVector subgraphSearch(DGraph& gD, QGraph& gQ) {
    // parallelize the search for candidates of gQ.begin()
    galois::InsertBag<NodeMatch> works;
    auto nQ = *(gQ.begin());
    for (auto c : gQ.getData(nQ).candidate)
      works.push_back(NodeMatch(nQ, c));

    MatchingVector report;
    galois::for_each(works, SubgraphSearchInternal(gD, gQ, report),
                     galois::loopname("search_for_each"),
                     galois::no_conflicts(), galois::no_pushes(),
                     galois::parallel_break());
    return report;
  }
};

// check if the first matching is correct
void verifyMatching(Matching& matching, DGraph& gD, QGraph& gQ) {
  bool isFailed = false;

  for (auto& nm1 : matching) {
    auto& dQ1 = gQ.getData(nm1.nQ);
    auto& dD1 = gD.getData(nm1.nD);

    if (dQ1.label != dD1.label) {
      isFailed = true;
      std::cerr << "label not match: gQ(" << dQ1.id << ") = " << dQ1.label;
      std::cerr << ", gD(" << dD1.id << ") = " << dD1.label << std::endl;
    }

    for (auto& nm2 : matching) {
      auto& dQ2 = gQ.getData(nm2.nQ);
      auto& dD2 = gD.getData(nm2.nD);

      // two distinct query nodes map to the same data node
      if (nm1.nQ != nm2.nQ && nm1.nD == nm2.nD) {
        isFailed = true;
        std::cerr << "inconsistent mapping to data node: gQ(" << dQ1.id;
        std::cerr << ") to gD(" << dD1.id << "), gQ(" << dQ2.id;
        std::cerr << ") to gD(" << dD2.id << ")" << std::endl;
      }

      // a query node mapped to different data nodes
      if (nm1.nQ == nm2.nQ && nm1.nD != nm2.nD) {
        isFailed = true;
        std::cerr << "inconsistent mapping from query node: gQ(" << dQ1.id;
        std::cerr << ") to gD(" << dD1.id << "), gQ(" << dQ2.id;
        std::cerr << ") to gD(" << dD2.id << ")" << std::endl;
      }

      // query edge not matched to data edge
      if (gQ.findEdgeSortedByDst(nm1.nQ, nm2.nQ) != gQ.edge_end(nm1.nQ) &&
          gD.findEdgeSortedByDst(nm1.nD, nm2.nD) == gD.edge_end(nm1.nD)) {
        isFailed = true;
        std::cerr << "edge not match: gQ(" << dQ1.id << " => " << dQ2.id;
        std::cerr << "), but no gD(" << dD1.id << " => " << dD2.id << ")"
                  << std::endl;
      }
    }
  }

  if (isFailed)
    GALOIS_DIE("Verification failed");
}

void reportMatchings(MatchingVector& report, DGraph& gD, QGraph& gQ) {
  auto output    = std::ofstream("report.txt");
  unsigned int i = 0;
  for (auto& m : report) {
    output << i << ": { ";
    for (auto& nm : m)
      output << "(" << gQ.getData(nm.nQ).id << ", " << gD.getData(nm.nD).id
             << ") ";
    output << "}" << std::endl;
    ++i;
  }
  output.close();
}

template <typename Algo>
void run() {
  DGraph gD;
  if (graphD.size()) {
    galois::graphs::readGraph(gD, graphD);
    std::cout << "Reading data graph..." << std::endl;
  } else
    GALOIS_DIE("Failed to read data graph");

  gD.sortAllEdgesByDst();
  gD.sortAllInEdgesByDst();

  if (rndSeedDByTime)
    rndSeedD = std::chrono::system_clock::now().time_since_epoch().count();
  std::cout << "rndSeedD: " << rndSeedD << std::endl;
  initializeGraph(gD, rndSeedD);
  std::cout << "data graph initialized" << std::endl;

  QGraph gQ;
  if (graphQ.size()) {
    galois::graphs::readGraph(gQ, graphQ);
    std::cout << "Reading query graph..." << std::endl;
  } else
    GALOIS_DIE("Failed to read query graph");

  gQ.sortAllEdgesByDst();
  gQ.sortAllInEdgesByDst();

  if (rndSeedQByTime)
    rndSeedQ = std::chrono::system_clock::now().time_since_epoch().count();
  std::cout << "rndSeedQ: " << rndSeedQ << std::endl;
  initializeGraph(gQ, rndSeedQ);
  std::cout << "query graph initialized" << std::endl;

  Algo algo;
  std::cout << "Running " << algo.name() << " Algorithm..." << std::endl;

  galois::StatTimer T;
  T.start();

  galois::StatTimer filterT("FilterCandidates");
  filterT.start();
  bool isSomeNodeUnmatched = algo.filterCandidates(gD, gQ);
  filterT.stop();

  if (isSomeNodeUnmatched) {
    T.stop();
    std::cout << "Some nodes have no candidates to match." << std::endl;
    return;
  }

  galois::StatTimer searchT("SubgraphSearch");
  searchT.start();
  currentlyFound.store(0);
  MatchingVector report = algo.subgraphSearch(gD, gQ);
  searchT.stop();

  T.stop();
  std::cout << "Found " << currentlyFound << " instance(s) of the query graph."
            << std::endl;
  if (currentlyFound) {
    reportMatchings(report, gD, gQ);
    for (auto& m : report)
      verifyMatching(m, gD, gQ);
    std::cout << "Verification succeeded" << std::endl;
  }
}

int main(int argc, char** argv) {
  galois::StatManager statManager;
  LonestarStart(argc, argv, name, desc, url);

  galois::StatTimer T("TotalTime");
  T.start();
  switch (algo) {
  case Algo::ullmann:
    run<UllmannAlgo>();
    break;
  case Algo::vf2:
    run<VF2Algo>();
    break;
  default:
    std::cerr << "Unknown algorithm\n";
    abort();
  }
  T.stop();

  return 0;
}
