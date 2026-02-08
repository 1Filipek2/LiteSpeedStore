#include "StorageEngine.hpp"
#include "FastTrace.hpp"
#include <thread>
#include <vector>
#include <iostream>

void heavyOperation(StorageEngine& db) {
    TRACE_SCOPE("HeavyOperation", db);
    
    std::this_thread::sleep_for(std::chrono::milliseconds(50)); //workload sim
}

int main() {
    StorageEngine metrics;

    std::cout << "Starting application with FastTrace..." << std::endl;

    {
        TRACE_SCOPE("TotalMainRuntime", metrics);
        
        for(int i = 0; i < 5; ++i) {
            heavyOperation(metrics);
        }
    }

    std::cout << "\nMeasured metrics in StorageEngine:" << std::endl;
    std::cout << "Operation time: " << metrics.get("HeavyOperation").value_or("N/A") << std::endl;
    std::cout << "Total runtime: " << metrics.get("TotalMainRuntime").value_or("N/A") << std::endl;

    return 0;
}