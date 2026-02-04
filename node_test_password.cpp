#include "node_manager.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <iostream>

class Random {
	uint64_t state;

    public:
	Random(uint64_t seed) : state(seed) {}

	uint32_t next_uint32() {
		state ^= state >> 12;
		state ^= state << 25;
		state ^= state >> 27;
		return static_cast<uint32_t>((state * 2685821657736338717ULL) >> 32);
	}
};

int8_t correct_password[4] = {-127, 28, 39, 127};

struct WordState {
	int8_t password[4] = {};
	int8_t decision[4] = {};
	bool dead= false;

	bool operator==(const WordState& other) const {
                return std::memcmp(password, &other.password, sizeof(password)) == 0;
	}

	double evaluate() {
		double score = 0.0;
		for (size_t i = 0; i < 4; ++i) {
			if (password[i] == correct_password[i]) {
				++score;
			}
			int diff = std::abs(password[i] - correct_password[i]);
			// if (diff <= 50) {
			// 	score += static_cast<double>(diff) / 50.0;
			// }
		}
		if (decision[3] == 0 && decision[2] == 0 && decision[1] == 0 && decision[0] == 0) {
			dead = true;
		}
		return score;
        }
};

struct WordHashFunc {
        uint64_t operator()(const WordState& state) const {
                uint64_t hash = 14695981039346656037ULL;
                for (size_t i = 0; i < 4; ++i) {
                        hash ^= static_cast<uint8_t>(state.password[i]);
                        hash *= 1099511628211ULL;
                }
                return hash;
        }
};


template <typename State>
struct CollisionFunc {
	static bool operator()(const State& a, const State& b) {
		return a == b;
	}
};

// get all possible moves for a 4 array password (moves -1, 0 , and 1) (all number combinations)
constexpr std::array<std::array<int8_t, 4>, 81> get_all_possible_moves() {
        std::array<std::array<int8_t, 4>, 81> moves = {};
        size_t index = 0;
        for (int8_t a = -1; a <= 1; ++a) {
                for (int8_t b = -1; b <= 1; ++b) {
                        for (int8_t c = -1; c <= 1; ++c) {
                                for (int8_t d = -1; d <= 1; ++d) {
                                        moves[index++] = {a, b, c, d};
                                }
                        }
                }
        }
        return moves;
}


int main() {
	constexpr int kMillisecondsPerMove = 100;
	ai::NodeTreeManager<WordState, WordHashFunc, CollisionFunc<WordState>> node_word;
	node_word.get_config().depth = 7;
	node_word.get_config().award_width = 25;
	Random rng(12345);
	WordState word_state;
        size_t attempts=  0;
	while (std::memcmp(word_state.password, correct_password, sizeof(correct_password)) != 0) {
		if (!node_word.try_advance()) {
			node_word.reset(word_state, 1);
		}
		auto now = std::chrono::high_resolution_clock::now();
		while (!node_word.is_releasable() || now - std::chrono::high_resolution_clock::now() < std::chrono::milliseconds(kMillisecondsPerMove)) {
			if (node_word.is_search_complete()) {
				break;
			}
			auto threads = node_word.get_tasks();
			for (auto& thread : threads) {
				auto do_tasks = [local_thread = std::move(thread), &node_word]() {
					for (auto& task : local_thread.tasks) {
						for (auto& parent : task.nodes) {
							if (parent->state.dead) {
								continue;
							}
							constexpr auto all_moves = get_all_possible_moves();
							for (const auto& move : all_moves) {
								auto node = node_word.allocate_new_node(local_thread.thread_id, parent);
								node->state = parent->state;
								std::memcpy(node->state.decision, move.data(), sizeof(node->state.decision));
								for (size_t i = 0; i < 4; ++i) {
									node->state.password[i] += move[i];
                                                                }
								double value = node->state.evaluate();
								node_word.push_new_node(task.depth + 1, node, value);
							}
						}
					}
				};
				do_tasks();
			}
			node_word.finalize();
		}
		auto best_state = node_word.get_best_state();
		++attempts;
		std::cout << "Attempt #" << attempts << std::endl;
		std::cout << "Applied decision: ";
                for (size_t i = 0; i < 4; ++i) {
                        word_state.password[i] += best_state->decision[i];
			std::cout << static_cast<int>(best_state->decision[i]) << " ";
		}
		std::cout << "\nCurrent password: ";
                for (size_t i = 0; i < 4; ++i) {
                        std::cout << static_cast<int>(word_state.password[i]) << " ";
                }
                std::cout << "\n\n";
	}
}