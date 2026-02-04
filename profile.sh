clang++ -std=c++20 -pg -o main node_test_sudoku.cpp -march=native && ./main
gprof main gmon.out > analysis.txt
cat analysis.txt