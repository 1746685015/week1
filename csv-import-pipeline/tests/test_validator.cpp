#include "csvp/validator.h"
#include "csvp/row.h"

#include <gtest/gtest.h>
#include <chrono>
#include <iostream>
#include <vector>
#include <string>

namespace {

TEST(Row, TypedConversions)
{
	csvp::Row row({
		"0", "", "N/A", "12abc", "-42", "2147483648", "1.5", "1e9999",
		"nan", "inf", "false", "maybe", " 7 "
	});

	int integerValue = 99;
	EXPECT_TRUE(row.tryToInt(0, integerValue));
	EXPECT_EQ(integerValue, 0);
	EXPECT_FALSE(row.tryToInt(1, integerValue));
	EXPECT_FALSE(row.tryToInt(2, integerValue));
	EXPECT_FALSE(row.tryToInt(3, integerValue));
	EXPECT_TRUE(row.tryToInt(4, integerValue));
	EXPECT_EQ(integerValue, -42);
	EXPECT_FALSE(row.tryToInt(5, integerValue));
	EXPECT_FALSE(row.tryToInt(12, integerValue));
	EXPECT_FALSE(row.tryToInt(99, integerValue));

	double doubleValue = 0.0;
	EXPECT_TRUE(row.tryToDouble(6, doubleValue));
	EXPECT_DOUBLE_EQ(doubleValue, 1.5);
	EXPECT_FALSE(row.tryToDouble(7, doubleValue));
	EXPECT_FALSE(row.tryToDouble(8, doubleValue));
	EXPECT_FALSE(row.tryToDouble(9, doubleValue));

	bool boolValue = true;
	EXPECT_TRUE(row.tryToBool(10, boolValue));
	EXPECT_FALSE(boolValue);
	EXPECT_FALSE(row.tryToBool(11, boolValue));
	EXPECT_FALSE(row.tryToBool(99, boolValue));

	EXPECT_EQ(row.toInt(2), 0);
	EXPECT_DOUBLE_EQ(row.toDouble(2), 0.0);
}

TEST(Row, ClearHidesPreviousFieldsAndReusesSlots)
{
	csvp::Row row;
	row.append("a long field value that owns dynamic storage");
	row.append("stale");
	row.setValid(false);

	row.clear();
	EXPECT_TRUE(row.empty());
	EXPECT_EQ(row.size(), 0U);
	EXPECT_TRUE(row.isValid());
	EXPECT_TRUE(row.toString(0U).empty());
	int value = 0;
	EXPECT_FALSE(row.tryToInt(0U, value));
	EXPECT_THROW((void)row.at(0U), std::out_of_range);

	row.append("fresh");
	EXPECT_FALSE(row.empty());
	ASSERT_EQ(row.size(), 1U);
	EXPECT_EQ(row[0U], "fresh");
	EXPECT_THROW((void)row.at(1U), std::out_of_range);

	std::string &directField = row.appendField();
	directField.append("direct");
	ASSERT_EQ(row.size(), 2U);
	EXPECT_EQ(row[1U], "direct");
}

// ============================================================
// Validator.TypeCheckString (PASS)
// ============================================================
TEST(Validator, TypeCheckString) {
    csvp::FieldValidator v;
    v.addRule({"name", csvp::FieldType::String, true});

    csvp::Row row({"Alice"});
    auto errors = v.validate(row);
    EXPECT_TRUE(errors.empty());
}

// ============================================================
// Validator.TypeCheckInt (PASS)
// ============================================================
TEST(Validator, TypeCheckInt) {
    csvp::FieldValidator v;
    v.addRule({"age", csvp::FieldType::Int, true});

    // Valid int
    csvp::Row row1({"25"});
    EXPECT_TRUE(v.validate(row1).empty());

    // Invalid int
    csvp::Row row2({"N/A"});
    auto errors2 = v.validate(row2);
    EXPECT_FALSE(errors2.empty());
    EXPECT_EQ(errors2[0].code, csvp::ErrorCode::TypeConversionError);
}

// ============================================================
// Validator.TypeCheckDouble (PASS)
// ============================================================
TEST(Validator, TypeCheckDouble) {
    csvp::FieldValidator v;
    v.addRule({"score", csvp::FieldType::Double, true});

    // Valid double
    csvp::Row row1({"95.5"});
    EXPECT_TRUE(v.validate(row1).empty());

    // Invalid double
    csvp::Row row2({"abc"});
    auto errors2 = v.validate(row2);
    EXPECT_FALSE(errors2.empty());
    EXPECT_EQ(errors2[0].code, csvp::ErrorCode::TypeConversionError);
}

// ============================================================
// Validator.RequiredField (PASS)
// ============================================================
TEST(Validator, RequiredField) {
    csvp::FieldValidator v;
    v.addRule({"name", csvp::FieldType::String, true});

    // Empty required field
    csvp::Row row1({""});
    auto errors1 = v.validate(row1);
    EXPECT_FALSE(errors1.empty());
    EXPECT_EQ(errors1[0].code, csvp::ErrorCode::RequiredFieldMissing);
}

// ============================================================
// Validator.RangeCheck (PASS)
// ============================================================
TEST(Validator, RangeCheck) {
    csvp::FieldValidator v;
    csvp::FieldRule rule;
    rule.name = "age";
    rule.type = csvp::FieldType::Int;
    rule.has_range = true;
    rule.min_val = 0;
    rule.max_val = 150;
    v.addRule(rule);

    // Within range
    csvp::Row row1({"25"});
    EXPECT_TRUE(v.validate(row1).empty());

    // Out of range
    csvp::Row row2({"200"});
    auto errors2 = v.validate(row2);
    EXPECT_FALSE(errors2.empty());
    EXPECT_EQ(errors2[0].code, csvp::ErrorCode::RangeError);
}

// ============================================================
// Validator.RegexCheck (PASS)
// ============================================================
TEST(Validator, RegexCheck) {
    csvp::FieldValidator v;
    csvp::FieldRule rule;
    rule.name = "email";
    rule.type = csvp::FieldType::String;
    rule.regex = R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})";
    v.addRule(rule);

    csvp::Row row1({"alice@example.com"});
    EXPECT_TRUE(v.validate(row1).empty());

    csvp::Row row2({"not-an-email"});
    auto errors2 = v.validate(row2);
    EXPECT_FALSE(errors2.empty());
    EXPECT_EQ(errors2[0].code, csvp::ErrorCode::RegexMismatch);
}

// ============================================================
// Validator.MultipleRules (PASS)
// ============================================================
TEST(Validator, MultipleRules) {
    csvp::FieldValidator v;
    v.addRule({"id", csvp::FieldType::Int, true});
    v.addRule({"name", csvp::FieldType::String, true});
    v.addRule({"score", csvp::FieldType::Double, false});

    csvp::Row row1({"1", "Alice", "95.5"});
    EXPECT_TRUE(v.validate(row1).empty());

    csvp::Row row2({"abc", "Bob", "bad"});
    auto errors2 = v.validate(row2);
    EXPECT_EQ(errors2.size(), 2u);
}

TEST(Validator, ReportsSchemaColumnMismatch)
{
	csvp::FieldValidator validator;
	validator.addRule({"id", csvp::FieldType::Int, true});
	validator.addRule({"name", csvp::FieldType::String, true});

	const csvp::Row extraColumnRow({"1", "Alice", "unexpected"});
	const std::vector<csvp::CellError> extraColumnErrors = validator.validate(extraColumnRow);
	ASSERT_EQ(extraColumnErrors.size(), 1U);
	EXPECT_EQ(extraColumnErrors[0].column_index, 2U);
	EXPECT_EQ(extraColumnErrors[0].raw_value, "unexpected");
	EXPECT_EQ(extraColumnErrors[0].code, csvp::ErrorCode::ColumnCountMismatch);

	const csvp::Row missingColumnRow({"1"});
	const std::vector<csvp::CellError> missingColumnErrors = validator.validate(missingColumnRow);
	ASSERT_EQ(missingColumnErrors.size(), 1U);
	EXPECT_EQ(missingColumnErrors[0].column_index, 1U);
	EXPECT_EQ(missingColumnErrors[0].column_name, "name");
	EXPECT_EQ(missingColumnErrors[0].code, csvp::ErrorCode::ColumnCountMismatch);
}

TEST(Validator, RejectsNumericOverflowAndNonFiniteValues)
{
	const csvp::FieldRule integerRule = {"value", csvp::FieldType::Int, true};
	csvp::FieldValidator integerValidator;
	integerValidator.addRule(integerRule);
	EXPECT_EQ(integerValidator.validateField(0U, "9223372036854775807", integerRule).code,
		csvp::ErrorCode::Ok);
	EXPECT_EQ(integerValidator.validateField(0U, "-9223372036854775808", integerRule).code,
		csvp::ErrorCode::Ok);
	EXPECT_EQ(integerValidator.validateField(0U, "9223372036854775808", integerRule).code,
		csvp::ErrorCode::TypeConversionError);
	EXPECT_EQ(integerValidator.validateField(0U, "-9223372036854775809", integerRule).code,
		csvp::ErrorCode::TypeConversionError);
	EXPECT_EQ(integerValidator.validateField(0U, " 1", integerRule).code,
		csvp::ErrorCode::TypeConversionError);

	const csvp::FieldRule doubleRule = {"value", csvp::FieldType::Double, true};
	csvp::FieldValidator doubleValidator;
	doubleValidator.addRule(doubleRule);
	EXPECT_EQ(doubleValidator.validateField(0U, "-0", doubleRule).code, csvp::ErrorCode::Ok);
	EXPECT_EQ(doubleValidator.validateField(0U, "+1.5", doubleRule).code, csvp::ErrorCode::Ok);
	for (const char *invalidValue : {"1e9999", "nan", "inf", "-inf", " 1.0"})
	{
		EXPECT_EQ(doubleValidator.validateField(0U, invalidValue, doubleRule).code,
			csvp::ErrorCode::TypeConversionError);
	}
}

TEST(Validator, NumericRangeIncludesExactBoundaries)
{
	csvp::FieldRule integerRule;
	integerRule.name = "integer";
	integerRule.type = csvp::FieldType::Int;
	integerRule.has_range = true;
	integerRule.min_val = -10.0;
	integerRule.max_val = 10.0;
	csvp::FieldValidator integerValidator;
	integerValidator.addRule(integerRule);
	EXPECT_EQ(integerValidator.validateField(0U, "-10", integerRule).code, csvp::ErrorCode::Ok);
	EXPECT_EQ(integerValidator.validateField(0U, "10", integerRule).code, csvp::ErrorCode::Ok);
	EXPECT_EQ(integerValidator.validateField(0U, "-11", integerRule).code,
		csvp::ErrorCode::RangeError);
	EXPECT_EQ(integerValidator.validateField(0U, "11", integerRule).code,
		csvp::ErrorCode::RangeError);

	csvp::FieldRule doubleRule;
	doubleRule.name = "double";
	doubleRule.type = csvp::FieldType::Double;
	doubleRule.has_range = true;
	doubleRule.min_val = -1.0;
	doubleRule.max_val = 1.0;
	csvp::FieldValidator doubleValidator;
	doubleValidator.addRule(doubleRule);
	EXPECT_EQ(doubleValidator.validateField(0U, "-1.0", doubleRule).code, csvp::ErrorCode::Ok);
	EXPECT_EQ(doubleValidator.validateField(0U, "1.0", doubleRule).code, csvp::ErrorCode::Ok);
	EXPECT_EQ(doubleValidator.validateField(0U, "1.0001", doubleRule).code,
		csvp::ErrorCode::RangeError);
}

TEST(Validator, RegexPerformanceWorkload)
{
	csvp::FieldRule rule;
	rule.name = "email";
	rule.type = csvp::FieldType::String;
	rule.regex = R"([a-zA-Z0-9._%+-]+@[a-zA-Z0-9.-]+\.[a-zA-Z]{2,})";
	csvp::FieldValidator validator;
	validator.addRule(rule);
	const csvp::Row row({"alice@example.com"});
	constexpr std::size_t ITERATION_COUNT = 50000U;

	const auto startTime = std::chrono::steady_clock::now();
	for (std::size_t iteration = 0; iteration < ITERATION_COUNT; ++iteration)
		ASSERT_TRUE(validator.validate(row).empty());
	const auto finishTime = std::chrono::steady_clock::now();
	const double elapsedMilliseconds =
		std::chrono::duration<double, std::milli>(finishTime - startTime).count();
	std::cout << "RegexPerformanceWorkload: " << ITERATION_COUNT
		<< " validations in " << elapsedMilliseconds << " ms" << std::endl;
}

TEST(Validator, ReportsInvalidRegexConfiguration)
{
	csvp::FieldRule rule;
	rule.name = "value";
	rule.regex = "[";
	csvp::FieldValidator validator;
	validator.addRule(rule);

	std::string errorMessage;
	EXPECT_TRUE(validator.getConfigurationError(errorMessage));
	EXPECT_NE(errorMessage.find("Invalid regex pattern"), std::string::npos);
	const csvp::CellError error = validator.validateField(0U, "value", rule);
	EXPECT_EQ(error.code, csvp::ErrorCode::RegexMismatch);
	EXPECT_EQ(error.message, errorMessage);
}

} // anonymous namespace
