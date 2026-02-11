#pragma once
#include <cassert>
#include <cstddef>
#include <deque>
#include <limits>
#include <stdexcept>
#include <vector>

#include "priority_queue.hpp"

namespace node {
	template <typename State>
	class NodeManager {
	    private:
		static_assert(sizeof(State) >= sizeof(size_t));

		struct Node {
			Node* parent;
			State state;

			void mark_pruned(const bool pruned) {
				size_t* prune_data = reinterpret_cast<size_t*>(&state);
				*prune_data = pruned ? std::numeric_limits<size_t>::max() : 0;
			}

			bool is_pruned() const {
				return *reinterpret_cast<const size_t*>(&state) == std::numeric_limits<size_t>::max();
			}

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
				return get_parent_at(n - 1);
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
				ret->mark_pruned(false);
				ret->parent = parent;
				return ret;
			}

			void deallocate(Node* node) {
				node->mark_pruned(true);
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

		using NodeValuePriorityQueue = utils::PriorityQueue<NodeValue, NodeValueCompare>;

		struct NodeTreeConfig {
			size_t depth = 7;
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
				assert(searched.size() == 1);
				assert(unsearched.empty());
				searched[0]->parent = nullptr;
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
						if (node->is_pruned()) {
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
					remove_losers(data, []( NodeValue& nv) { return nv.node; });
					unsearched.import_container(std::move(data));
				}
				remove_losers(searched, []( Node* node) { return node; });
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
					assert(depths[i].searched.empty());
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
			for (NodeDepth& depth : depths) {
				depth.searched.clear();
				depth.unsearched.clear();
			}
			depths.resize(config.depth + 1);
			Node* root = memory.allocate(nullptr);
			root->state = current_state;
			depths.front().push(root, 0);
		}

		bool prune() {
			size_t first_active_depth_index = get_first_active_depth_index();
			size_t last_active_depth_index = get_last_active_depth_index();

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

			for (size_t i = first_active_depth_index; i <= last_active_depth_index; ++i) {
				NodeDepth& depth = depths[i];
				depth.cleanup(memory);
			}
			return true;
		}

	    public:
		NodeTreeConfig& get_config() {
			return config;
		}

		void prepare_tree(const State& current_state) {
			if (config.depth < depths.size()) {
				reset(current_state);
				return;
			}
			depths.resize(config.depth);
			Node* root = get_root();
			if (root == nullptr) {
				reset(current_state);
				return;
			}
			if (root->state != current_state) {
				reset(current_state);
				return;
			}
			memory.deallocate(root);
			for (size_t i = 0; i < depths.size() - 1; ++i) {
				depths[i] = std::move(depths[i + 1]);
			}
			depths.front().filter(get_best_node()->get_first_parent(), memory);
			depths.front().make_root();
			depths.back().clear();
			for (size_t i = 1; i < depths.size() - 1; ++i) {
				depths[i].cleanup(memory);
			}
		}

		State* get_task() {
			if (memory.is_limit_reached(config.node_limit)) {
				if (!prune()) {
					return nullptr;
				}
			}
			size_t check_count = 0;
			while (check_count != depths.size() && depths[node_cursor.depth].unsearched.empty()) {
				++check_count;
				++node_cursor.depth;
				if (node_cursor.depth >= depths.size()) {
					node_cursor.depth = 0;
				}
			}
			if (check_count == depths.size()) {
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

		size_t get_total_node_count() const {
			return memory.size();
		}
	};

} // namespace node