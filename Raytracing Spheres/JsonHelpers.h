#pragma once

#include "nlohmann/json.hpp"

using ordered_json_f = nlohmann::basic_json<nlohmann::ordered_map, std::vector, std::string, bool, int64_t, uint64_t, float>;

#define TO_JSON_FUNCTION_PROTOTYPE(Type) inline void to_json(ordered_json_f& nlohmann_json_j, const Type& nlohmann_json_t)

#define TO_JSON(Member) nlohmann_json_j[#Member] = nlohmann_json_t.Member;
#define TO_JSON_FUNCTION_BODY(...) NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(TO_JSON, __VA_ARGS__))
#define TO_JSON_FUNCTION(Type, ...) TO_JSON_FUNCTION_PROTOTYPE(Type) { TO_JSON_FUNCTION_BODY(__VA_ARGS__) }

#define TO_JSON1(Key, Member) nlohmann_json_j[Key] = nlohmann_json_t.Member;
#define TO_JSON1_PAIR(Pair) TO_JSON1 Pair
#define TO_JSON1_FUNCTION_BODY(...) NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(TO_JSON1_PAIR, __VA_ARGS__))
#define TO_JSON1_FUNCTION(Type, ...) TO_JSON_FUNCTION_PROTOTYPE(Type) { TO_JSON1_FUNCTION_BODY(__VA_ARGS__) }

#define FROM_JSON_FUNCTION_PROTOTYPE(Type) inline void from_json(const ordered_json_f& nlohmann_json_j, Type& nlohmann_json_t)

#define FROM_JSON(Member) if (const auto value = nlohmann_json_j.find(#Member); value != cend(nlohmann_json_j)) value->get_to(nlohmann_json_t.Member);
#define FROM_JSON_FUNCTION_BODY(...) NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FROM_JSON, __VA_ARGS__))
#define FROM_JSON_FUNCTION(Type, ...) FROM_JSON_FUNCTION_PROTOTYPE(Type) { FROM_JSON_FUNCTION_BODY(__VA_ARGS__) }

#define FROM_JSON1(Key, Member) if (const auto value = nlohmann_json_j.find(Key); value != cend(nlohmann_json_j)) value->get_to(nlohmann_json_t.Member);
#define FROM_JSON1_PAIR(Pair) FROM_JSON1 Pair
#define FROM_JSON1_FUNCTION_BODY(...) NLOHMANN_JSON_EXPAND(NLOHMANN_JSON_PASTE(FROM_JSON1_PAIR, __VA_ARGS__))
#define FROM_JSON1_FUNCTION(Type, ...) FROM_JSON_FUNCTION_PROTOTYPE(Type) { FROM_JSON1_FUNCTION_BODY(__VA_ARGS__) }

#define JSON_CONVERSION_FUNCTIONS(Type, ...) TO_JSON_FUNCTION(Type, __VA_ARGS__) FROM_JSON_FUNCTION(Type, __VA_ARGS__)
#define FRIEND_JSON_CONVERSION_FUNCTIONS(Type, ...) friend TO_JSON_FUNCTION(Type, __VA_ARGS__) friend FROM_JSON_FUNCTION(Type, __VA_ARGS__)

#define JSON_CONVERSION1_FUNCTIONS(Type, ...) TO_JSON1_FUNCTION(Type, __VA_ARGS__) FROM_JSON1_FUNCTION(Type, __VA_ARGS__)
#define FRIEND_JSON_CONVERSION1_FUNCTIONS(Type, ...) friend TO_JSON1_FUNCTION(Type, __VA_ARGS__) friend FROM_JSON1_FUNCTION(Type, __VA_ARGS__)
