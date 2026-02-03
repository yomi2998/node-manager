#pragma once

#include "priority_queue.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <new>
#include <unordered_map>
#include <vector>

#ifndef PREVENT_FALSE_SHARING
#define PREVENT_FALSE_SHARING alignas(std::hardware_destructive_interference_size)
#endif

namespace ai {

	template <typename State, typename HashFunc, typename CollisionFunc>
	class NodeTreeManager {
	    private:
		struct Node {
			Node* parent;
			Node* child;
			Node* sibling;
			uint32_t thread_id;
			uint32_t total_value;
			State state;

			void award(const uint32_t value) {
				if (parent == nullptr)
					return;
				total_value += value;
				parent->award(value);
			}

			Node* get_first_parent() {
				if (parent != nullptr && parent->parent == nullptr) {
					return this;
				}
				return parent->get_first_parent();
			}

			const Node* get_first_parent() const {
				if (parent != nullptr && parent->parent == nullptr) {
					return this;
				}
				return parent->get_first_parent();
			}

			void sanity_check() const {
				if (parent != nullptr) {
					bool found = false;
					for (Node* sibling = parent->child; sibling != nullptr; sibling = sibling->sibling) {
						if (sibling == this) {
							found = true;
							break;
						}
					}
					if (!found) {
						printf("Sanity check failed: Node not found in parent's child list\n");
						std::abort();
					}
					parent->sanity_check();
				}
			}
		};

		struct NodeValue {
			Node* node;
			double value;
		};

		class PREVENT_FALSE_SHARING NodeMemory {
		    private:
			std::deque<Node> node_storage;
			Node* free_head = nullptr;
			size_t free_count = 0;

		    public:
			Node* allocate(Node* parent) {
				Node* ret;
				if (free_head == nullptr) {
					node_storage.emplace_back();
					ret = &node_storage.back();
				} else {
					ret = free_head;
					free_head = free_head->parent;
					--free_count;
				}
				ret->child = nullptr;
				ret->sibling = nullptr;
				ret->total_value = 0;
				ret->parent = parent;
				if (parent->child != nullptr) {
					ret->sibling = parent->child;
				}
				parent->child = ret;
				return ret;
			}

			Node* allocate_root() {
				Node* ret;
				if (free_head == nullptr) {
					node_storage.emplace_back();
					ret = &node_storage.back();
				} else {
					ret = free_head;
					free_head = free_head->parent;
					--free_count;
				}
				ret->child = nullptr;
				ret->sibling = nullptr;
				ret->total_value = 0;
				ret->parent = nullptr;
				return ret;
			}

			void deallocate(Node* node) {
				node->parent = free_head;
				node->thread_id = std::numeric_limits<uint32_t>::max();
				free_head = node;
				++free_count;
			}

			size_t size() const {
				return node_storage.size();
			}

			size_t remaining() const {
				return free_count;
			}
		};

		class NodeMemoryLanes {
		    private:
			// potential issue: some lane has high memory usage
			std::vector<NodeMemory> lanes;

		    public:
			bool is_limit_reached(const size_t limit) const {
				size_t total = 0;
				for (const auto& lane : lanes) {
					total += lane.size();
				}
				return total >= limit;
			}

			// check if some memory lane actually has high memory usage, probably unneeded
			size_t check_memory_min_max_diff() const {
				size_t min = std::numeric_limits<size_t>::max();
				size_t max = std::numeric_limits<size_t>::min();
				for (const auto& lane : lanes) {
					min = std::min(lane.size(), min);
					max = std::max(lane.size(), max);
				}
				return max - min;
			}

			std::vector<int> get_free_count() const {
				std::vector<int> counts;
				for (const auto& lane : lanes) {
					counts.push_back(static_cast<int>(lane.remaining()));
				}
				return counts;
			}

			Node* allocate(const size_t thread_id, Node* parent) {
				Node* ret = lanes[thread_id].allocate(parent);
				ret->thread_id = thread_id;
				return ret;
			}

			Node* allocate_root() {
				return lanes[0].allocate_root();
			}

			void deallocate(Node* node) {
				for (Node* child = node->child; child != nullptr; child = child->sibling) {
					deallocate(child);
				}
				lanes[node->thread_id].deallocate(node);
			}

			void reset(const size_t thread_count, Node* root_node) {
				if (!root_node) {
					lanes.clear();
				} else {
					deallocate(root_node);
				}
				lanes.resize(thread_count);
			}

			size_t get_threads() const {
				return lanes.size();
			}
		};

		struct NodeValueCompare {
			static bool operator()(const NodeValue& left, const NodeValue& right) {
				return left.value < right.value;
			}
		};

		struct NodePruneCompare {
			static bool operator()(const Node* left, const Node* right) {
				return left->total_value > right->total_value;
			}
		};

		struct NodeTreeConfig {
			size_t depth = 7;
			size_t depth_task_size = 16; // algorithm won't exactly follow this, maximum is probably (depth_task_size * 2) - 1
			size_t node_limit = 100000;  // soft limit, will continue allocating until can_release returns true
			size_t prune_width = 1;
			size_t award_width = 25;
		};

		struct OutgoingDepthTasks {
			std::vector<Node*> nodes;
			size_t depth;
		};

		struct ThreadTasks {
			std::vector<OutgoingDepthTasks> tasks;
			size_t thread_id;

			ThreadTasks& operator=(ThreadTasks&& other) {
				tasks = std::move(other.tasks);
				thread_id = other.thread_id;
				return *this;
			}

			ThreadTasks(ThreadTasks&& other) {
				*this = std::move(other);
			}

			ThreadTasks() = default;
		};

		using NodePQ = PriorityQueue<NodeValue, NodeValueCompare>;

		struct PREVENT_FALSE_SHARING DepthTasks {
			NodePQ tasks;
			std::unordered_map<uint64_t, std::vector<Node*>> transposition_table;
		};

	    private:
		NodeMemoryLanes lanes;

		std::vector<DepthTasks> pending_depths;
		NodeTreeConfig config;
		Node* root = nullptr;

		HashFunc hash_func;
		CollisionFunc collision_func;

		void cleanup_depth(const size_t index) {
			auto remove_orphans = []<typename Container, typename GetElement>(Container& container, GetElement get_element) {
				for (size_t i = 0; i < container.size();) {
					if (get_element(container[i])->thread_id == std::numeric_limits<uint32_t>::max()) {
						container[i] = std::move(container.back());
						container.pop_back();
					} else {
						++i;
					}
				}
			};

			// cleanup pending tasks
			std::vector<NodeValue> data = pending_depths[index].tasks.export_container();
			remove_orphans(data, [](const NodeValue& nv) { return nv.node; });
			pending_depths[index].tasks.import_container(std::move(data));

			// cleanup transposition table
			auto& tt = pending_depths[index].transposition_table;
			for (auto it = tt.begin(); it != tt.end();) {
				auto& vec = it->second;
				remove_orphans(vec, [](Node* node) { return node; });
				if (vec.empty()) {
					it = tt.erase(it);
				} else {
					++it;
				}
			}
		}

		void cleanup_all_depths() {
			for (size_t i = 0; i < pending_depths.size(); ++i) {
				cleanup_depth(i);
			}
		}

		const DepthTasks* get_last_active_depth() const {
			for (size_t i = pending_depths.size(); i-- > 0;) {
				if (!pending_depths[i].tasks.empty()) {
					return &pending_depths[i];
				}
			}
			return nullptr;
		}

		const Node* get_best_node()const {
			const DepthTasks* last_active_depth = get_last_active_depth();
			if (last_active_depth == nullptr) {
				return nullptr;
			}
			return last_active_depth->tasks.top().node;
		}

	    public:
		NodeTreeConfig& get_config() {
			return config;
		}

		Node* allocate_new_node(const size_t thread_id, Node* parent) {
			// return lanes.allocate(thread_id, parent);
			auto node = lanes.allocate(thread_id, parent);
			node->sanity_check();
			return node;
		}

		void push_new_node(const size_t depth, Node* node, const double value) {
			// check transposition table
			const uint64_t hash = hash_func(node->state);
			auto& bucket = pending_depths[depth].transposition_table[hash];
			for (Node* existing_node : bucket) {
				if (collision_func(existing_node->state, node->state)) {
					return; // duplicate found, discard new node
				}
			}
			bucket.push_back(node);
			pending_depths[depth].tasks.push({node, value});
		}

		std::vector<ThreadTasks> get_tasks() {
			std::vector<ThreadTasks> threads;
			threads.resize(lanes.get_threads());
			for (size_t i = 0; i < threads.size(); ++i) {
				threads[i].thread_id = i;
			}

			std::vector<int> free_counts = lanes.get_free_count();
			std::vector<size_t> thread_task_counts(threads.size(), 0);

			size_t current_depth = 0;

			auto get_thread_with_most_free = [&]() -> size_t {
				size_t max_thread = 0;
				int max_free = std::numeric_limits<int>::min();
				for (size_t t = 0; t < threads.size(); ++t) {
					if (free_counts[t] > max_free) {
						max_free = free_counts[t];
						max_thread = t;
					}
				}
				return max_thread;
			};

			auto find_best_thread = [&]() -> size_t {
				size_t best_thread = 0;
				float best_score = std::numeric_limits<float>::lowest();
				for (size_t t = 0; t < threads.size(); ++t) {
					float score = static_cast<float>(free_counts[t]) - static_cast<float>(thread_task_counts[t]);
					if (score > best_score) {
						best_score = score;
						best_thread = t;
					}
				}
				return best_thread;
			};

			size_t current_thread = get_thread_with_most_free();
			size_t pending_depths_size = pending_depths.size() - 1; // last depth is finalization only
			while (current_depth < pending_depths_size) {
				auto& pending = pending_depths[current_depth].tasks;
				auto& thread = threads[current_thread];

				if (pending.empty()) {
					++current_depth;
					continue;
				}

				size_t tasks_added_this_depth = 0;

				while (!pending.empty() && tasks_added_this_depth < config.depth_task_size) {
					NodeValue nv = pending.top();
					pending.pop();

					if (thread.tasks.empty() || thread.tasks.back().depth != current_depth) {
						thread.tasks.push_back(OutgoingDepthTasks{{}, current_depth});
					}

					thread.tasks.back().nodes.push_back(nv.node);
					++tasks_added_this_depth;
					++thread_task_counts[current_thread];
				}
				++current_depth;

				// Now that we are moving to a new depth, check if we should hand off to another thread.
				if (thread_task_counts[current_thread] >= config.depth_task_size) {
					current_thread = find_best_thread();
				}
			}

			// trim unused threads but keep thread_id intact
			threads.erase(
			    std::remove_if(threads.begin(), threads.end(), [](const ThreadTasks& t) { return t.tasks.empty(); }),
			    threads.end());

			return threads;
		}

		void reset(const State& root_state, const size_t thread_count) {
			lanes.reset(thread_count, root);
			pending_depths.resize(config.depth + 1);
			for (auto& depth : pending_depths) {
				depth.tasks.clear();
				depth.transposition_table.clear();
			}
			root = lanes.allocate_root();
			root->state = root_state;
			pending_depths[0].tasks.push({root, 0.0});
		}

		bool is_search_complete() const {
			if (lanes.is_limit_reached(config.node_limit)) {
				return false;
			}
			auto end = pending_depths.end() - 1; // last depth is finalization only
			for (auto it = pending_depths.begin(); it != end; ++it) {
				if (!it->tasks.empty()) {
					return false;
				}
			}
			return true;
		}

		bool is_releasable() const {
			if (pending_depths.back().tasks.empty()) {
				return is_search_complete();
			}
			return true;
		}

		void finalize() {
			auto& last_depth_tasks = pending_depths.back().tasks;
			if (last_depth_tasks.empty()) {
				return;
			}
			std::vector<NodeValue> top_k_nodes;
			top_k_nodes.reserve(config.award_width);
			while (!last_depth_tasks.empty() && top_k_nodes.size() < config.award_width) {
				top_k_nodes.push_back(last_depth_tasks.top());
				last_depth_tasks.pop();
			}
			size_t award_size = top_k_nodes.size();
			for (const auto& nv : top_k_nodes) {
				nv.node->award(award_size--);
				last_depth_tasks.push(nv);
			}
			PriorityQueue<Node*, NodePruneCompare> frontier_nodes;
			frontier_nodes.reserve(config.prune_width);

			// get top k worst nodes
			// starts at root's children
			// condition: if root's children size <= 1, prune root's grandchildren
			Node* cursor = root;
			// navigate to first depth with multiple children
			while (cursor->child != nullptr && cursor->child->sibling == nullptr) {
				cursor = cursor->child;
			}
			if (cursor->child == nullptr) {
				// no children at all, nothing to prune
				return;
			}
			// now cursor->child has multiple siblings
			for (Node* child = cursor->child; child != nullptr; child = child->sibling) {
				frontier_nodes.push(child);
			}

			// prune worst nodes
			size_t target_frontier_size = config.prune_width > frontier_nodes.size() ? 1 : config.prune_width;
			while (frontier_nodes.size() > target_frontier_size) {
				Node* worst_node = frontier_nodes.top();
				frontier_nodes.pop();
				lanes.deallocate(worst_node);
			}

			// assign back to cursor's children
			cursor->child = nullptr;
			while (!frontier_nodes.empty()) {
				Node* top = frontier_nodes.top();
				frontier_nodes.pop();
				top->sibling = cursor->child;
				cursor->child = top;
			}

			cleanup_all_depths(); // remove orphans (zombie nodes) from all depths
		}

		const State* get_best_state() const {
			const Node* best_node = get_best_node();
			if (best_node != nullptr) {
				best_node = best_node->get_first_parent();
				return &best_node->state;
			}
			return nullptr;
		}

		bool try_advance() {
			if (root == nullptr || root->child == nullptr) {
				return false;
			}
			// advance root to best child
			Node* best_child = nullptr;
			for (Node* child = root->child; child != nullptr; child = child->sibling) {
				if (best_child == nullptr || child->total_value > best_child->total_value) {
					if (best_child != nullptr) {
						lanes.deallocate(best_child);
					}
					best_child = child;
				}
			}
			root->child = nullptr; // important: detach best child from root to avoid double free
			lanes.deallocate(root);
			root = best_child;
			root->parent = nullptr;

			// shift pending depths
			for (size_t i = 0; i < pending_depths.size() - 1; ++i) {
				pending_depths[i] = std::move(pending_depths[i + 1]);
			}
			pending_depths.back().tasks.clear();
			pending_depths.back().transposition_table.clear();
			return true;
		}
	};
} // namespace ai

#undef PREVENT_FALSE_SHARING