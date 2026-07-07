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
	}

	/**
     * Build failure links (BFS). Must be called after all addPattern() calls
     * and before any match() call.
     */
	void build()
	{
		std::queue<Node *> q;
		// Level 1: children of root → failure = root
		for (auto &n : root_.next) {
			if (n) {
				n->fail = &root_;
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
				// Propagate output from failure link
				if (child->fail->output)
					child->output = child->fail->output;
				q.push(child);
			}
		}
	}

	/**
     * Match a single string against the automaton.
     * Returns the id of the longest matching pattern, or 0 if no match.
     * O(len) single pass — vs O(N * len) for N strcmp calls.
     */
	int match(const char *text) const
	{
		if (!text)
			return 0;
		Node *cur = const_cast<Node *>(&root_);
		for (const char *p = text; *p; p++) {
			int idx = static_cast<unsigned char>(*p);
			while (cur != &root_ && !cur->next[idx])
				cur = cur->fail;
			if (cur->next[idx])
				cur = cur->next[idx];
			if (cur->output)
				return cur->output;
		}
		return cur->output;
	}

	int match(const std::string &text) const
	{
		return match(text.c_str());
	}

    private:
	struct Node {
		Node *next[256] = {}; // ASCII
		Node *fail = nullptr;
		int output = 0; // pattern id (0 = none)
	};
	Node root_;
};

#endif // AHOCORASICK_H
