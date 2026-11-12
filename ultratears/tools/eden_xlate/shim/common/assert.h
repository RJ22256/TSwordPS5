// Stand-in for Eden's common/assert.h for the offline shader tool: a failed assertion throws, so one bad shader
// fails its own translation instead of ending the batch.
#pragma once
#include <stdexcept>
#include <string>
#include "common/logging.h"

#define ASSERT(cond) do { if (!(cond)) throw std::logic_error("assertion failed: " #cond); } while (0)
#define ASSERT_MSG(cond, ...) do { if (!(cond)) throw std::logic_error(std::string("assertion failed: " #__VA_ARGS__)); } while (0)
#define DEBUG_ASSERT(cond) do {} while (0)
#define DEBUG_ASSERT_MSG(cond, ...) do {} while (0)
#define UNREACHABLE() throw std::logic_error("unreachable")
#define UNREACHABLE_MSG(...) throw std::logic_error(std::string("unreachable: " #__VA_ARGS__))
#define UNIMPLEMENTED() throw std::logic_error("unimplemented")
#define UNIMPLEMENTED_MSG(...) throw std::logic_error(std::string("unimplemented: " #__VA_ARGS__))
#define ASSERT_OR_EXECUTE(cond, body) do { if (!(cond)) { body } } while (0)
