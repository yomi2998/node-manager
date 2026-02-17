#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "priority_queue.hpp"

namespace noir {
	template <typename State, typename StateEqual, typename StateHash>
	class NodeManager {
	    private:
		static_assert(sizeof(State) >= sizeof(size_t));

		struct Node {
			Node* parent;
			State state;
			bool pruned;

			const Node* get_first_parent() const {
				if (parent == nullptr) {
					return nullptr;
				}
				if (parent != nullptr && parent->parent == nullptr) {
					return this;
				}
				return parent->get_first_parent();
			}

			const Node* get_parent_at(const size_t n) const {
				if (n == 0) {
					return this;
				}
				assert(parent != nullptr);
				return parent->get_parent_at(n - 1);
			}
		};

		class NodeMemory {
		    private:
			std::deque<Node> node_storage;
			Node* free_head = nullptr;
			size_t cursor = 0;
			size_t free_count = 0;

			Node* allocate_raw() {
				Node* ret;
				if (free_head != nullptr) {
					ret = free_head;
					free_head = free_head->parent;
					--free_count;
				} else if (cursor < node_storage.size()) {
					ret = &node_storage[cursor++];
					--free_count;
				} else {
					node_storage.emplace_back();
					ret = &node_storage.back();
					++cursor;
				}
				return ret;
			}

		    public:
			void reset() {
				free_head = nullptr;
				cursor = 0;
				free_count = node_storage.size();
			}

			size_t size() const {
				return node_storage.size() - free_count;
			}

			size_t remaining() const {
				return free_count;
			}

			bool is_limit_reached(const size_t limit) const {
				return size() >= limit;
			}

			Node* allocate(Node* parent) {
				Node* ret = allocate_raw();
				ret->pruned = false;
				ret->parent = parent;
				return ret;
			}

			void deallocate(Node* node) {
				node->pruned = true;
				node->parent = free_head;
				free_head = node;
				++free_count;
			}
		};

		struct NodeValue {
			Node* node;
			double value;
		};

		struct NodeValueCompare {
			static bool operator()(const NodeValue& left, const NodeValue& right) {
				return left.value < right.value;
			}
		};

		using NodeValuePriorityQueue = PriorityQueue<NodeValue, NodeValueCompare>;

		struct NodeTreeConfig {
			size_t depth = 7;
			size_t prune_depth_limit = 0;
			size_t node_limit = 100000; // soft limit
		};

		struct NodeCursor {
			Node* cursor = nullptr;
			Node* allocated_node = nullptr;
			size_t depth = 0;
		};

		struct NodeDepth {
			NodeValuePriorityQueue unsearched;
			std::vector<Node*> searched;

			void make_root() {
				assert(size() == 1);
				if (!searched.empty()) {
					searched[0]->parent = nullptr;
				} else {
					unsearched.top().node->parent = nullptr;
				}
			}

			void push(Node* node, double value) {
				unsearched.push({node, value});
			}

			Node* get_unsearched_node() {
				Node* ret = unsearched.top().node;
				unsearched.pop();
				searched.emplace_back(ret);
				return ret;
			}

			size_t size() const {
				return unsearched.size() + searched.size();
			}

			bool empty() const {
				return unsearched.empty() && searched.empty();
			}

			void cleanup(NodeMemory& memory) {
				if (empty()) {
					return;
				}
				auto remove_orphans = [&memory]<typename Container, typename GetElement>(Container& container, GetElement get_element) {
					for (size_t i = 0; i < container.size();) {
						Node* node = get_element(container[i]);
						if (node->parent->pruned) {
							memory.deallocate(node);
							container[i] = std::move(container.back());
							container.pop_back();
						} else {
							++i;
						}
					}
				};
				if (!unsearched.empty()) {
					std::vector<NodeValue> data = unsearched.export_container();
					remove_orphans(data, [](NodeValue& nv) { return nv.node; });
					unsearched.import_container(std::move(data));
				}
				remove_orphans(searched, [](Node* node) { return node; });
			}

			void filter(const Node* survivor, NodeMemory& memory) {
				if (empty()) {
					return;
				}
				auto remove_losers = [&]<typename Container, typename GetElement>(Container& container, GetElement get_element) {
					for (size_t i = 0; i < container.size();) {
						Node* node = get_element(container[i]);
						if (node != survivor) {
							memory.deallocate(node);
							container[i] = std::move(container.back());
							container.pop_back();
						} else {
							++i;
						}
					}
				};
				if (!unsearched.empty()) {
					std::vector<NodeValue> data = unsearched.export_container();
					remove_losers(data, [](NodeValue& nv) { return nv.node; });
					unsearched.import_container(std::move(data));
				}
				remove_losers(searched, [](Node* node) { return node; });
			}

			void clear() {
				unsearched.clear();
				searched.clear();
			}
		};

	    private:
		NodeMemory memory;
		NodeCursor node_cursor;
		std::vector<NodeDepth> depths;
		NodeTreeConfig config;
		size_t total_searched;
		size_t total_collision;

		std::unordered_map<uint64_t, Node*> transposition_table;
		StateEqual state_equal;
		StateHash state_hash;

		size_t get_first_active_depth_index() const {
			for (size_t i = 0; i < depths.size(); ++i) {
				if (depths[i].size() > 1) {
					return i;
				}
			}
			return std::numeric_limits<size_t>::max();
		}

		size_t get_last_active_depth_index() const {
			for (size_t i = depths.size(); i-- > 0;) {
				if (!depths[i].empty()) {
					return i;
				}
			}
			return std::numeric_limits<size_t>::max();
		}

		Node* get_best_node() {
			size_t index = get_last_active_depth_index();
			if (index == std::numeric_limits<size_t>::max()) {
				return nullptr;
			}
			if (depths[index].unsearched.empty()) {
				return nullptr;
			}
			return depths[index].unsearched.top().node;
		}

		Node* get_root() {
			std::vector<Node*>& searched_vec = depths.front().searched;
			if (searched_vec.empty()) {
				return nullptr;
			}
			assert(searched_vec.size() == 1);
			return searched_vec[0];
		}

		void reset(const State& current_state) {
			memory.reset();
			transposition_table.clear();
			for (NodeDepth& depth : depths) {
				depth.searched.clear();
				depth.unsearched.clear();
			}
			depths.resize(config.depth + 1);
			Node* root = memory.allocate(nullptr);
			root->state = current_state;
			depths.front().push(root, 0);
		}

		void cleanup(const size_t start, const size_t end) {
			for (size_t i = start; i < end; ++i) {
				NodeDepth& depth = depths[i];
				depth.cleanup(memory);
			}
			for (auto it = transposition_table.begin(); it != transposition_table.end();) {
				if (it->second->pruned) {
					it = transposition_table.erase(it);
				} else {
					++it;
				}
			}
		}

		bool prune() {
			if (config.prune_depth_limit == 0) {
				return false;
			}

			size_t first_active_depth_index = get_first_active_depth_index();
			size_t last_active_depth_index = get_last_active_depth_index();

			if (first_active_depth_index > config.prune_depth_limit) {
				return false;
			}
			if (first_active_depth_index == last_active_depth_index) {
				return false;
			}
			if (last_active_depth_index == std::numeric_limits<size_t>::max()) {
				throw std::runtime_error("node_limit is too low"); // maybe can remove if never triggers
			}

			size_t first_and_last_depth_index_diff = last_active_depth_index - first_active_depth_index;
			const Node* best_node = depths[last_active_depth_index].unsearched.top().node;
			best_node = best_node->get_parent_at(first_and_last_depth_index_diff);

			NodeDepth& first_active_depth = depths[first_active_depth_index];
			first_active_depth.filter(best_node, memory);

			cleanup(first_active_depth_index, last_active_depth_index + 1);
			return true;
		}

		void reset_metrics() {
			total_searched = 0;
			total_collision = 0;
		}

	    public:
		NodeTreeConfig& get_config() {
			return config;
		}

		bool verify_state() {
			if (node_cursor.allocated_node == nullptr || node_cursor.allocated_node->pruned) {
				return false;
			}
			uint64_t hash = state_hash(node_cursor.allocated_node->state);
			if (!transposition_table.try_emplace(hash, node_cursor.allocated_node).second) {
				++total_collision;
				memory.deallocate(node_cursor.allocated_node);
				return false;
			}
			return true;
		}

		void prepare_tree(const State& current_state) {
			reset_metrics();
			if (depths.size() <= config.depth) {
				reset(current_state);
				return;
			}
			depths.resize(config.depth + 1);
			Node* root = get_root();
			if (root == nullptr) {
				reset(current_state);
				return;
			}
			const Node* best_leaf = get_best_node();
			if (best_leaf == nullptr) {
				reset(current_state);
				return;
			}
			const Node* best_parent = best_leaf->get_first_parent();
			if (!state_equal(best_parent->state, current_state)) {
				reset(current_state);
				return;
			}
			memory.deallocate(root);
			for (size_t i = 0; i < depths.size() - 1; ++i) {
				depths[i] = std::move(depths[i + 1]);
			}
			depths.front().filter(best_parent, memory);
			depths.front().make_root();
			depths.back().clear();
			cleanup(1, depths.size() - 1);
		}

		void increment_depth_counter() {
			++node_cursor.depth;
			if (node_cursor.depth >= depths.size() - 1) {
				node_cursor.depth = 0;
			}
		}

		State* get_task() {
			if (memory.is_limit_reached(config.node_limit)) {
				if (!prune()) {
					return nullptr;
				}
			}
			size_t check_count = 0;
			size_t last_depth_counter = node_cursor.depth;
			while (check_count != depths.size() && depths[node_cursor.depth].unsearched.empty()) {
				++check_count;
				increment_depth_counter();
			}
			if (check_count == depths.size()) {
				node_cursor.depth = last_depth_counter;
				return nullptr;
			}
			node_cursor.cursor = depths[node_cursor.depth].get_unsearched_node();
			return &node_cursor.cursor->state;
		}

		State* get_new_state() {
			node_cursor.allocated_node = memory.allocate(node_cursor.cursor);
			return &node_cursor.allocated_node->state;
		}

		void report_result(const double value) {
			++total_searched;
			assert(node_cursor.depth + 1 != depths.size());
			depths[node_cursor.depth + 1].push(node_cursor.allocated_node, value);
		}

		const State* get_result() const {
			size_t last_depth_index = get_last_active_depth_index();
			if (depths[last_depth_index].unsearched.empty()) {
				return nullptr;
			}
			return &depths[last_depth_index].unsearched.top().node->get_first_parent()->state;
		}

		bool are_depths_populated() const {
			size_t last_depth_index = get_last_active_depth_index();
			if (last_depth_index == depths.size() - 1) {
				return true;
			}
			return depths[last_depth_index].unsearched.empty();
		}

		size_t get_total_node_count() const {
			return memory.size();
		}

		size_t get_total_searched_count() const {
			return total_searched;
		}

		size_t get_total_collision_count() const {
			return total_collision;
		}
	};

} // namespace noir