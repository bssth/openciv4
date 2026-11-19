#pragma once
#include "Pitboss.h"
namespace oc4
{
class AssetResolver;
}
namespace oc4::host
{
class GameSession;
class Utility;
void runPitboss(const pb::Config &, GameSession &, Utility &);
std::string pitbossIdentity(const AssetResolver &, const std::vector<std::filesystem::path> &mapDirs);
} // namespace oc4::host
