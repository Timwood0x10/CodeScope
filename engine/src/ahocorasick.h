#ifndef AHOCORASICK_H
#define AHOCORASICK_H

#include <cstring>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

/**
 * Aho-Corasick multi-pattern string matching automaton.
 *
 * Build once, query many: O(n + m + k) where n = text length, m = total
 * pattern length, k = number of matches. Replaces chains of N strcmp() calls
 * (O(N * len)) with a single linear scan (O(len)).
 *
 * Usage:
 *   ACAutomaton ac;
 *   ac.addPattern("function_declaration", 1);
 *   ac.addPattern("class_declaration", 2);
 *   ac.build();
 *   int id = ac.match("class_declaration"); // returns 2
 */
class ACAutomaton {
    public:
	ACAutomaton() = default;

	~ACAutomaton()
	{
		// Free all dynamically allocated nodes
		std::queue<Node *> to_free;
		for (auto &n : root_.next) {
			if (n)
				to_free.push(n);
		}
		while (!to_free.empty()) {
			Node *cur = to_free.front();
			to_free.pop();
			for (auto &n : cur->next) {
				if (n)
					to_free.push(n);
			}
			delete cur;
		}
	}

	/** Add a pattern with an associated integer id (returned on match). */
	void addPattern(const std::string &pattern, int id)
	{
		if (pattern.empty())
			return;
		Node *cur = &root_;
		for (char c : pattern) {
			int idx = static_cast<unsigned char>(c);
			if (!cur->next[idx])
				cur->next[idx] = new Node();
			cur = cur->next[idx];
		}
		cur->output = id;
		cur->output_len = static_cast<int>(pattern.size());
	}

	/**
     * Build failure links (BFS). Must be called after all addPattern() calls
     * and before any match() call.
     */
	void build()
	{
		std::queue<Node *> q;
		// Level 1: children of root → failure = root. Root has no
		// output, so out_link stays nullptr.
		for (auto &n : root_.next) {
			if (n) {
				n->fail = &root_;
				n->out_link = nullptr;
				q.push(n);
			}
		}
		// BFS for deeper levels
		while (!q.empty()) {
			Node *cur = q.front();
			q.pop();
			for (int i = 0; i < 256; i++) {
				Node *child = cur->next[i];
				if (!child)
					continue;
				// Find failure link
				Node *f = cur->fail;
				while (f && !f->next[i])
					f = f->fail;
				child->fail = f ? f->next[i] : &root_;
				// Build dictionary suffix link (out_link) to the
				// nearest ancestor in the fail chain that carries
				// its own output. We do NOT overwrite child->output
				// — the previous implementation did
				// `child->output = child->fail->output`, which
				// silently replaced the long pattern's id with the
				// shorter suffix pattern's id.
				Node *fail_node = child->fail;
				if (fail_node != &root_ && fail_node->output) {
					child->out_link = fail_node;
				} else {
					child->out_link = fail_node->out_link;
				}
				q.push(child);
			}
		}
	}

	/**
     * Match a single string against the automaton.
     * Returns the id of the longest matching pattern, or 0 if no match.
     * Walks the out_link chain at each position so suffix-pattern
     * matches are not lost, and tracks the longest pattern length seen
     * across the whole scan. O(len) single pass — vs O(N * len) for N
     * strcmp calls.
     */
	int match(const char *text) const
	{
		if (!text)
			return 0;
		Node *cur = const_cast<Node *>(&root_);
		int best_id = 0;
		int best_len = 0;
		for (const char *p = text; *p; p++) {
			int idx = static_cast<unsigned char>(*p);
			while (cur != &root_ && !cur->next[idx])
				cur = cur->fail;
			if (cur->next[idx])
				cur = cur->next[idx];
			// Walk the dictionary suffix chain: cur first (longest
			// pattern ending here), then progressively shorter
			// suffix patterns via out_link. Track the longest
			// match across all positions.
			for (Node *o = cur; o != nullptr; o = o->out_link) {
				if (o->output && o->output_len > best_len) {
					best_id = o->output;
					best_len = o->output_len;
				}
			}
		}
		return best_id;
	}

	int match(const std::string &text) const
	{
		return match(text.c_str());
	}

    private:
	struct Node {
		Node *next[256] = {}; // ASCII
		Node *fail = nullptr;
		// Dictionary suffix link: points to the nearest node in the
		// fail chain that has its own output. Forms a singly-linked
		// list of all patterns that are suffixes of this node's
		// string. nullptr when no such node exists.
		Node *out_link = nullptr;
		int output = 0; // pattern id (0 = none)
		int output_len = 0; // length of the pattern at this node
	};
	Node root_;
};

#endif // AHOCORASICK_H
