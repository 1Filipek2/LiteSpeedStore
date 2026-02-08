# LiteSpeedStore

I built this project to get my hands dirty with modern C++ and to see how fast I could make a simple in-memory storage engine. It’s small, it’s fast, and it doesn't leak memory.

## What I was practicing

- **Modern C++ (17+)**: Using `std::optional`.
- **Speed & Efficiency**: Playing with move semantics and `std::unique_ptr` to keep things snappy and avoid useless copying.
- **Not Crashing**: Added a `std::mutex` so that if I ever use multiple threads, they won't fight over the data.
- **Lazy Profiling**: Built a custom RAII Timer that does all the boring time-tracking for me automatically.

Basically, I wanted to see how "pro" I could make a key-value store while keeping the code clean enough that I wouldn't hate myself looking at it a week later.

I plan to extend this project further in the future as I experiment with more features and optimizations.
