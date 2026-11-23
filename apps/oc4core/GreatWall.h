#pragma once

// The Great Wall on the map, as the original engine lays it (observed in the original game and
// in the wall it keeps in its saves):
//  - The game core asks for it once, CvDLLEngineIFaceBase::AddGreatWall(city), when a building
//    that is an area border obstacle is built. The engine takes the city's owner's territory
//    joined to the city's plot (plots side by side, sea plots of the owner too) as it is then,
//    and walls its edge. Later changes to the borders, and a load, leave the wall where it is.
//  - Only one wall: while one stands, AddGreatWall changes nothing (CvCity calls it again for
//    a building it already has). The game core never asks to remove it (CvCity's count of other
//    border obstacles leaves out the one being removed).
//  - The edge is followed with the territory on its right, from the plot south of the city
//    where the territory ends. An edge is walled where both its plots are land of the city's
//    landmass (CvPlot::getAreaIdForGreatWall): the coast and the borders across the sea are
//    left open, so a wall is a closed loop or pieces that end at the coast.
// OpenCiv4 doesn't keep the wall in its saves (the original keeps it in its own section of the
// save, in a form not fully known): after a load it is laid again along the borders as they are.

#include <array>
#include <cstddef>
#include <vector>

class CvCity;

namespace oc4::host
{
	struct GreatWallChain
	{
		bool closed = false;
		std::vector<std::array<int, 2>> corners; // plot corners (plot x, y is the corner at its south-west)
	};

	struct GreatWall
	{
		int cityX = -1, cityY = -1;
		std::vector<GreatWallChain> chains;
	};

	// The wall the engine lays for the city now (its territory as it is).
	GreatWall traceGreatWall(const CvCity& city);
}
