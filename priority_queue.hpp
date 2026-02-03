#pragma once
#include <queue>

namespace ai {
	template <typename T, typename Compare>
	class PriorityQueue : public std::priority_queue<T, std::vector<T>, Compare> {
	    public:
		using Container = std::vector<T>;
		PriorityQueue() = default;

		void reserve(const size_t size) {
			this->c.reserve(size);
		}

		void clear() {
			this->c.clear();
		}

		explicit PriorityQueue(const Compare& comp)
		    : std::priority_queue<T, Container, Compare>(comp) {}

		PriorityQueue(const Compare& comp, const Container& cont)
		    : std::priority_queue<T, Container, Compare>(comp, cont) {}

		Container export_container() {
			return std::move(this->c);
		}

		void import_container(Container&& new_data) {
			this->c = std::move(new_data);
			std::make_heap(this->c.begin(), this->c.end(), this->comp);
		}
	};
} // namespace ai