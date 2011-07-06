/** GMetis -*- C++ -*-
 * @file
 * @section License
 *
 * Galois, a framework to exploit amorphous data-parallelism in irregular
 * programs.
 *
 * Copyright (C) 2011, The University of Texas at Austin. All rights reserved.
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
 *
 * @author Xin Sui <xinsui@cs.utexas.edu>>
 */

#ifndef HEM_H_
#define HEM_H_

class HEMMatcher{
private:
	int maxVertexWeight;
	GGraph* graph;
	GGraph* coarseGraph;
	MetisGraph* metisGraph;
public:
	HEMMatcher(MetisGraph* metisGraph, GGraph* coarseGraph, int maxVertexWeight) {
		this->coarseGraph=coarseGraph;
		this->metisGraph = metisGraph;
		this->graph=metisGraph->getGraph();
		this->maxVertexWeight=maxVertexWeight;
	}

	void match(GNode node) {
		MetisNode& nodeData = node.getData(Galois::Graph::CHECK_CONFLICT);
		if (metisGraph->isMatched(nodeData.getNodeId())) {
			return;
		}
		GNode matchNode = node;
		int maxwgt = -1;
		for (GGraph::neighbor_iterator jj = graph->neighbor_begin(node, Galois::Graph::CHECK_CONFLICT), eejj = graph->neighbor_end(node, Galois::Graph::CHECK_CONFLICT); jj != eejj; ++jj) {
			GNode neighbor = *jj;
			MetisNode& neighMNode = neighbor.getData(Galois::Graph::NONE);
			int edgeData = graph->getEdgeData(node, jj, Galois::Graph::NONE);
			if (!metisGraph->isMatched(neighMNode.getNodeId()) && maxwgt < edgeData
					&& nodeData.getWeight() + neighMNode.getWeight() <= maxVertexWeight) {
				maxwgt = edgeData;
				matchNode = neighbor;
			}
		}

		MetisNode& matchNodeData = matchNode.getData(Galois::Graph::NONE);
//		nodeData.setMatch(matchNode);
//		maxNodeData.setMatch(node);
		metisGraph->setMatch(nodeData.getNodeId(), matchNode);

		int weight = nodeData.getWeight();
		if (node != matchNode) {
			metisGraph->setMatch(matchNodeData.getNodeId(), node);
			weight += matchNodeData.getWeight();
		}

		GNode newNode = coarseGraph->createNode(MetisNode(weight));
		coarseGraph->addNode(newNode, Galois::Graph::NONE);
		metisGraph->setCoarseGraphMap(nodeData.getNodeId(), newNode);
		if(matchNode!=node){
			metisGraph->setCoarseGraphMap(matchNodeData.getNodeId(), newNode);
		}
	}
};

#endif /* HEM_H_ */
