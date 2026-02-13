#include "StorageEngine.hpp"
#include <cassert>
#include <iostream>
#include <filesystem>
#include <cmath>

void runTest() {
    std::string dbPath = "test_persist.wal";
    // Clean start
    if (std::filesystem::exists(dbPath)) std::filesystem::remove(dbPath);

    std::cout << "Starting Persistence Test..." << std::endl;

    {
        StorageEngine db(dbPath);
        db.set("key1", "val1", 10.0);
        db.set("key1", "val2", 20.0);
        assert(db.historyCount("key1") == 2);
        std::cout << "[Pass] Run 1: Write records." << std::endl;
    }
    // db destroyed, WAL closed.

    {
        StorageEngine db(dbPath);
        // Should recover
        size_t count = db.historyCount("key1");
        if (count != 2) {
            std::cerr << "Expected 2 records, got " << count << std::endl;
            exit(1);
        }
        
        auto val = db.get("key1");
        if (!val || *val != "val2") {
            std::cerr << "Expected 'val2', got " << (val ? *val : "null") << std::endl;
            exit(1);
        }

        double avg = db.getAverage("key1");
        if (std::abs(avg - 15.0) > 0.001) {
             std::cerr << "Expected avg 15.0, got " << avg << std::endl;
             exit(1);
        }
        
        std::cout << "[Pass] Run 2: Recovery successful." << std::endl;
        
        db.set("key1", "val3", 30.0);
        db.remove("key1");
    }
    
    {
        StorageEngine db(dbPath);
        // Should be empty for key1
        if (db.historyCount("key1") != 0) {
             std::cerr << "Expected 0 records after delete, got " << db.historyCount("key1") << std::endl;
             exit(1);
        }
        std::cout << "[Pass] Run 3: Delete persisted." << std::endl;
    }
    
    std::cout << "All persistence tests passed!" << std::endl;
    std::filesystem::remove(dbPath);
}

int main() {
    runTest();
    return 0;
}
