#ifndef COMMUNITY_DETECTION_H
#define COMMUNITY_DETECTION_H

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../store/store.h"

namespace query
{

/**
 * Community detection result for a single node.
 */
struct CommunityNode {
	uint64_t node_id;
	uint64_t community_id;
	std::string name;
	int node_type;
	std::string file_path;
};

/**
 * Detected community with its member nodes and inter-community edges.
 */
struct Community {
	uint64_t id;
	std::string label; // Most common file/module name in this community
	std::vector<CommunityNode> members;
	int member_count;
};

/**
 * Run label-propagation community detection on the code graph.
 *
 * The algorithm:
 *   1. Each graph node starts in its own community.
 *   2. Iteratively: each node adopts the most frequent community label
 *      among its neighbors. Ties broken by smallest community ID.
 *   3. Repeat until no node changes community.
 *
 * @param project_id  The project to analyze.
 * @param store       Initialized GraphStore.
 * @param max_members Maximum members per community in output (default 10).
 *                    Only used when include_members=true.
 *                    Set to 0 for all members (WARNING: may produce large token output).
 * @param max_communities Maximum number of communities to return (default 20).
 *                        Set to 0 for all communities.
 * @param include_members If true, include member list in each community.
 *                        If false (default), only return {id, label, member_count} summary.
 */
std::string detectCommunities(uint64_t project_id, store::GraphStore *store,
			      int max_members = 10, int max_communities = 20,
			      bool include_members = false);

} // namespace query

#endif // COMMUNITY_DETECTION_H
