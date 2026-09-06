#include "StorageEngine.hpp"
#include "FastTrace.hpp"
#include <thread>
#include <iostream>

void heavyOperation(StorageEngine& db)
{
    TRACE_SCOPE("HeavyOperation", db);

    std::this_thread::sleep_for(std::chrono::milliseconds(50)); // workload sim
}

int main()
{
    StorageEngine metrics = StorageEngine::makeInMemory();

    std::cout << "Starting application with FastTrace..." << '\n';

    {
        TRACE_SCOPE("TotalMainRuntime", metrics);

        for (int i = 0; i < 5; ++i)
        {
            heavyOperation(metrics);
        }
    }

    std::cout << "\nMeasured metrics in StorageEngine:" << '\n';
    std::cout << "Operation time: " << metrics.get("HeavyOperation").value_or("N/A") << '\n';
    std::cout << "Average time: " << metrics.getAverage("HeavyOperation").value_or(0.0) << " ms" << '\n';
    std::cout << "Total runtime: " << metrics.get("TotalMainRuntime").value_or("N/A") << '\n';

    return 0;
}