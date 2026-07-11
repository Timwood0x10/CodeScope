#ifndef ENGINE_INTERNAL_H
#define ENGINE_INTERNAL_H

#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

#include "engine.h"
#include "store/store.h"
#include "query/query_engine.h"
#include "parser/parser.h"
#include "ir/ir.h"
#include "ir/ir_translator.h"
#include "graph/graph_types.h"
#include "graph/graph_builder.h"
#include "query/graph_query.h"
#include "query/impact_analysis.h"
// community_detection removed — Phase 0 cut
#include "lsp/lsp_client.h"
// ─── Global Singletons ───────────────────────────────────────────
//
// Shared across all engine_*.cpp translation units.
// Initialized by engine_init() and cleaned up by engine_shutdown().
// Using unique_ptr for exception-safe memory management.

extern std::unique_ptr<store::GraphStore> g_store;
extern std::unique_ptr<query::QueryEngine> g_query;
extern std::unique_ptr<Parser> g_parser;

// ─── Internal Helpers ────────────────────────────────────────────
//
// Not part of the public API; used internally by engine_*.cpp files.

std::string readFile(const char *path);
std::string jsonEscape(const std::string &s);
std::string simpleHash(const std::string &s);
const char *detectLanguage(const char *file_path);
char *dupString(const std::string &s);

// ─── Path Helpers ─────────────────────────────────────────────────
// Cross-platform path separator check: '/' on Unix, '/' and '\\' on
// Windows. Using std::filesystem::path for full path manipulation is
// preferred, but for simple single-character checks this is lighter.
inline bool isPathSep(char c)
{
	return c == '/' || c == '\\';
}

// ─── Scanner Helpers ─────────────────────────────────────────────
// Only used within engine_scanner.cpp; declared there.

// ================================================================
// BoundedQueue: 线程安全的有界队列（生产者-消费者）
// ================================================================
//
// 用于 parse workers → single writer 的异步流水线。
// capacity = 2 * worker_count，DB 写慢时 worker 自然阻塞（背压）。
// push/pop 均为 O(1)，内部以 std::queue 实现。

template <typename T> class BoundedQueue {
    public:
	explicit BoundedQueue(size_t capacity)
		: capacity_(capacity > 0 ? capacity : 1)
	{
	}

	/**
	 * 推送一个元素到队列尾部。
	 * 队列满时阻塞，直到 writer pop 出元素腾出空间。
	 * 线程安全。
	 */
	void push(T value)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		not_full_.wait(lock, [this]() {
			return queue_.size() < capacity_ || done_;
		});
		if (done_)
			return;
		queue_.push(std::move(value));
		not_empty_.notify_one();
	}

	/**
	 * 从队列头部弹出一个元素。
	 * 队列空时阻塞，直到 worker push 或 markDone 被调用。
	 * @param value 输出参数，接收弹出的元素。
	 * @return true 成功弹出，false 队列已关闭且为空。
	 */
	bool pop(T &value)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		not_empty_.wait(lock,
				[this]() { return !queue_.empty() || done_; });
		if (queue_.empty())
			return false;
		value = std::move(queue_.front());
		queue_.pop();
		not_full_.notify_one();
		return true;
	}

	/**
	 * 批量弹出：最多弹出 max_count 个元素。
	 * 队列空时阻塞。
	 * @param values 输出参数，接收弹出的元素。
	 * @param max_count 最大弹出数量。
	 * @return 实际弹出的数量。
	 */
	size_t popBatch(std::vector<T> &values, size_t max_count)
	{
		std::unique_lock<std::mutex> lock(mutex_);
		not_empty_.wait(lock,
				[this]() { return !queue_.empty() || done_; });
		size_t count = 0;
		while (!queue_.empty() && count < max_count) {
			values.push_back(std::move(queue_.front()));
			queue_.pop();
			count++;
		}
		if (count > 0)
			not_full_.notify_all();
		return count;
	}

	/**
	 * 获取当前队列大小。
	 */
	size_t size() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return queue_.size();
	}

	/**
	 * 标记队列为已完成。所有阻塞的 push/pop 将返回。
	 */
	void markDone()
	{
		std::lock_guard<std::mutex> lock(mutex_);
		done_ = true;
		not_empty_.notify_all();
		not_full_.notify_all();
	}

	bool isDone() const
	{
		std::lock_guard<std::mutex> lock(mutex_);
		return done_;
	}

    private:
	size_t capacity_;
	bool done_ = false;
	mutable std::mutex mutex_;
	std::condition_variable not_full_;
	std::condition_variable not_empty_;
	std::queue<T> queue_;
};

#endif // ENGINE_INTERNAL_H
