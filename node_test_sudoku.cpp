#include "node_manager.hpp"
#include "thread.hpp"
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

struct SudokuDecision {
	uint8_t x;
	uint8_t y;
	uint8_t number;
};

struct SudokuState {
	// col, row
	uint8_t board[9][9] = {};
	SudokuDecision decision = {};
	SudokuDecision last_decision = {};

	bool operator==(const SudokuState& other) const {
		return std::memcmp(board, other.board, sizeof(board)) == 0;
	}

	int get_column_match_count(const size_t column) const {
		int match[10] = {};
		for (size_t row = 0; row < 9; ++row) {
			++match[board[column][row]];
		}
		int result = 0;
		for (int i = 1; i < 10; ++i) {
			result += match[i] != 0;
		}
		return result;
	}

	int get_row_match_count(const size_t row) const {
		int match[10] = {};
		for (size_t column = 0; column < 9; ++column) {
			++match[board[column][row]];
		}
		int result = 0;
		for (int i = 1; i < 10; ++i) {
			result += match[i] != 0;
		}
		return result;
	}

	int get_block_match_count(const size_t block) const {
		int match[10] = {};
		size_t col_start = (block * 3) % 9;
		size_t row_start = (block / 3) * 3;
		size_t col_end = col_start + 3;
		size_t row_end = row_start + 3;
		for (size_t i = col_start; i < col_end; ++i) {
			for (size_t j = row_start; j < row_end; ++j) {
				++match[board[i][j]];
			}
		}
		int result = 0;
		for (int i = 1; i < 10; ++i) {
			result += match[i] != 0;
		}
		return result;
	}

	int get_zero_count() const {
		int count = 0;
		for (size_t x = 0; x < 9; ++x) {
			for (size_t y = 0; y < 9; ++y) {
				if (board[x][y] == 0) {
					++count;
				}
			}
		}
		return count;
	}

	bool is_solved() const {
		for (size_t i = 0; i < 9; ++i) {
			size_t matches = 0;
			matches += get_block_match_count(i);
			matches += get_row_match_count(i);
			matches += get_column_match_count(i);
			if (matches != 27) {
				return false;
			}
		}
		return true;
	}

	double evaluate() {
		double score = 0.0;
		for (size_t i = 0; i < 9; ++i) {
			score += get_block_match_count(i);
			score += get_row_match_count(i);
			score += get_column_match_count(i);
		}
		score -= get_zero_count() * 99999;
		if (std::memcmp(&decision, &last_decision, sizeof(SudokuDecision)) == 0) {
			score = -99999; // discourage repeating same decision
		}
		return score;
	}
};

struct SudokuHashFunc {
	uint64_t operator()(const SudokuState& state) const {
		uint64_t hash = 0xcbf29ce484222325;
		for (size_t i = 0; i < 9; ++i) {
			for (size_t j = 0; j < 9; ++j) {
				hash ^= state.board[i][j];
				hash *= 0x100000001b3;
			}
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

// get all possible moves from 9x9 board with 1-9 num
constexpr std::array<SudokuDecision, 9 * 9 * 9> get_all_possible_moves() {
	std::array<SudokuDecision, 9 * 9 * 9> moves = {};
	size_t index = 0;
	for (size_t x = 0; x < 9; ++x) {
		for (size_t y = 0; y < 9; ++y) {
			for (size_t n = 1; n < 10; ++n) {
				SudokuDecision decision;
				decision.x = x;
				decision.y = y;
				decision.number = n;
				moves[index++] = decision;
			}
		}
	}
	return moves;
}

int main() {
	constexpr int kMillisecondsPerMove = 10;
	thread::ThreadPool thread_pool;
	thread_pool.init(1);
	ai::NodeTreeManager<SudokuState, SudokuHashFunc, CollisionFunc<SudokuState>> node_sudoku;
	node_sudoku.get_config().depth = 5;
	node_sudoku.get_config().depth_task_size = 1;
	node_sudoku.get_config().beam_width = 250;
	node_sudoku.get_config().award_width = 250;
	node_sudoku.get_config().prune_width = 500;
	node_sudoku.get_config().node_limit = 1000000;
	Random rng(12345);
	SudokuState sudoku_state;
	size_t attempts = 0;
	while (!sudoku_state.is_solved()) {
		if (!node_sudoku.try_advance()) {
			node_sudoku.reset(sudoku_state, thread_pool.size());
		}
		auto now = std::chrono::high_resolution_clock::now();
		 while (std::chrono::high_resolution_clock::now() - now < std::chrono::milliseconds(kMillisecondsPerMove)) {
		//while(true){
			if (node_sudoku.is_search_complete()) {
				break;
			}
			auto threads = node_sudoku.get_tasks();
			for (auto& thread : threads) {
				auto do_tasks = [local_thread = std::move(thread), &node_sudoku]() mutable {
					for (auto& task : local_thread.tasks) {
						for (auto& parent : task.nodes) {
							constexpr auto all_moves = get_all_possible_moves();
							for (const auto& move : all_moves) {
								if (parent->state.board[move.x][move.y] == move.number) {
									continue; // skip if already set
								}
								auto node = node_sudoku.allocate_new_node(local_thread.thread_id, parent);
								node->state = parent->state;
								node->state.last_decision = node->state.decision;
								node->state.decision = move;
								node->state.board[move.x][move.y] = move.number;
								double value = node->state.evaluate();
								node_sudoku.push_new_node(task.depth + 1, node, value);
							}
						}
					}
				};

				do_tasks();
				//thread_pool.enqueue(do_tasks);
			}
			//thread_pool.wait();
			node_sudoku.finalize();
		}
		auto best_state = node_sudoku.get_best_state();
		++attempts;
		std::cout << "Attempt #" << attempts << std::endl;
		std::cout << "Total nodes generated: " << node_sudoku.get_total_node_count() << std::endl;
		if (best_state != nullptr) {
			// sudoku_state.last_decision = sudoku_state.decision;
			// sudoku_state.decision = best_state->decision;
			// sudoku_state.board[best_state->decision.x][best_state->decision.y] = best_state->decision.number;
			sudoku_state = *best_state;
			std::cout << "Applied decision: x: " << static_cast<int>(best_state->decision.x)
			          << ";y: " << static_cast<int>(best_state->decision.y)
			          << ";number: " << static_cast<int>(best_state->decision.number) << std::endl;
		}
		std::cout << "Current board: \n\n";
		for (size_t y = 0; y < 9; ++y) {	 // Outer loop: Rows
			for (size_t x = 0; x < 9; ++x) { // Inner loop: Columns
				std::cout << " " << static_cast<int>(sudoku_state.board[x][y]);
			}
			std::cout << "\n";
		}
		std::cout << "\n";
	}
}