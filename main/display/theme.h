#pragma once
#include <string>

class Theme {
public:
    Theme(const std::string& name) : name_(name) {}
    virtual ~Theme() = default;

    inline std::string name() const { return name_; }

private:
    std::string name_;
};