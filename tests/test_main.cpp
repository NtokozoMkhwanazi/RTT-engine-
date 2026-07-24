/**
 * RTT-Engine Unit Test Runner
 * 
 * This file is the entry point for all unit tests.
 * Run with: make test
 */

#include <gtest/gtest.h>

// Forward declarations for test registration
// Add your test suite includes here

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    
    std::cout << "\n========================================\n";
    std::cout << "  RTT-Engine Unit Tests\n";
    std::cout << "========================================\n\n";
    
    int result = RUN_ALL_TESTS();
    
    std::cout << "\n========================================\n";
    if (result == 0) {
        std::cout << "  All tests passed!\n";
    } else {
        std::cout << "  Some tests failed.\n";
    }
    std::cout << "========================================\n\n";
    
    return result;
}
