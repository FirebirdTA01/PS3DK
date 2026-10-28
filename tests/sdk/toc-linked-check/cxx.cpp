#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <stdexcept>
static std::map<std::string, double> table;
struct Base { virtual ~Base() {} virtual double f(double x) const { return x * 1.5; } };
struct Der : Base { double f(double x) const override { return x / 3.25 + 0.125; } };
int main(int argc, char **) {
    std::vector<Base *> v{new Base, new Der};
    for (int i = 0; i < 4; ++i) table["k" + std::to_string(i)] = v[i & 1]->f(i + argc);
    try { if (argc > 5) throw std::runtime_error("x"); } catch (const std::exception &e) { std::puts(e.what()); }
    std::printf("%s %f\n", "done", table["k3"]);
    return 0;
}
