// Copyright Epic Games, Inc. All Rights Reserved.
#include "Chaos/GraphColoring.h"
#include "Chaos/Array.h"

using namespace Chaos;

TArray<TArray<int32>> FGraphColoring::ComputeGraphColoring(const TArray<TVector<int32, 2>>& Graph, const TDynamicParticles<Chaos::FReal, 3>& InParticles)
{
	TArray<TArray<int32>> ColorGraph;
	TArray<FGraphNode> Nodes;
	TArray<FGraphEdge> Edges;
	Nodes.SetNum(InParticles.Size());
	Edges.SetNum(Graph.Num());
	int32 MaxColor = -1;

	for (int32 i = 0; i < Graph.Num(); ++i)
	{
		const TVector<int32, 2>& Constraint = Graph[i];
		Edges[i].FirstNode = Constraint[0];
		Edges[i].SecondNode = Constraint[1];
		Nodes[Constraint[0]].Edges.Add(i);
		Nodes[Constraint[1]].Edges.Add(i);
	}

	TSet<int32> ProcessedNodes;
	TArray<int32> NodesToProcess;

	for (uint32 ParticleNodeIndex = 0; ParticleNodeIndex < InParticles.Size(); ++ParticleNodeIndex)
	{
		const bool bIsParticleDynamic = InParticles.InvM(ParticleNodeIndex) != 0;
		if (ProcessedNodes.Contains(ParticleNodeIndex) || !bIsParticleDynamic)
		{
			continue;
		}

		NodesToProcess.Add(ParticleNodeIndex);

		while (NodesToProcess.Num())
		{
			const int32 NodeIndex = NodesToProcess.Last();
			FGraphNode& GraphNode = Nodes[NodeIndex];

			NodesToProcess.SetNum(NodesToProcess.Num() - 1, /*bAllowShrinking=*/false);
			ProcessedNodes.Add(NodeIndex);

			for (const int32 EdgeIndex : GraphNode.Edges)
			{
				FGraphEdge& GraphEdge = Edges[EdgeIndex];

				// If edge has been colored skip it
				if (GraphEdge.Color >= 0)
				{
					continue;
				}

				// Get index to the other node on the edge
				int32 OtherNodeIndex = INDEX_NONE;
				if (GraphEdge.FirstNode == NodeIndex)
				{
					OtherNodeIndex = GraphEdge.SecondNode;
				}
				if (GraphEdge.SecondNode == NodeIndex)
				{
					OtherNodeIndex = GraphEdge.FirstNode;
				}

				// Find next color that is not used already at this node
				while (GraphNode.UsedColors.Contains(GraphNode.NextColor))
				{
					GraphNode.NextColor++;
				}
				int32 ColorToUse = GraphNode.NextColor;

				// Exclude colors used by the other node (but still allow this node to use them for other edges)
				if (OtherNodeIndex != INDEX_NONE)
				{
					FGraphNode& OtherNode = Nodes[OtherNodeIndex];

					const bool bIsOtherGraphNodeDynamic = InParticles.InvM(OtherNodeIndex) != 0;
					if (bIsOtherGraphNodeDynamic)
					{
						while (OtherNode.UsedColors.Contains(ColorToUse) || GraphNode.UsedColors.Contains(ColorToUse))
						{
							ColorToUse++;
						}
					}
				}

				// Assign color and set as used at this node
				MaxColor = FMath::Max(ColorToUse, MaxColor);
				GraphNode.UsedColors.Add(ColorToUse);
				GraphEdge.Color = ColorToUse;

				// Bump color to use next time, but only if we weren't forced to use a different color by the other node
				if ((ColorToUse == GraphNode.NextColor) && bIsParticleDynamic == true)
				{
					GraphNode.NextColor++;
				}

				if (ColorGraph.Num() <= MaxColor)
				{
					ColorGraph.SetNum(MaxColor + 1);
				}
				ColorGraph[GraphEdge.Color].Add(EdgeIndex);

				if (OtherNodeIndex != INDEX_NONE)
				{
					FGraphNode& OtherGraphNode = Nodes[OtherNodeIndex];
					const bool bIsOtherGraphNodeDynamic = InParticles.InvM(OtherNodeIndex) != 0;
					if (bIsOtherGraphNodeDynamic)
					{
						// Mark other node as not allowing use of this color
						if (bIsParticleDynamic)
						{
							OtherGraphNode.UsedColors.Add(GraphEdge.Color);
						}

						// Queue other node for processing
						if (!ProcessedNodes.Contains(OtherNodeIndex))
						{
							NodesToProcess.Add(OtherNodeIndex);
						}
					}
				}
			}
		}
	}

	return ColorGraph;
}
