#include "tag_recovery_journal.h"

#include <iostream>
#include <stdexcept>

using nekorox::component::TagRecoveryJournal;
namespace json = sonare::util::json;
int checks = 0;
void require(bool okay, const char* label) { ++checks; if (!okay) throw std::runtime_error(label); }
std::string file(std::string suffix = {})
{
    return "{\"path\":\"track\",\"backupPath\":\"backup\",\"sourceSha256\":\"" + std::string(64, 'a') +
        "\",\"postWriteSha256\":\"" + std::string(64, 'b') + "\",\"outcome\":\"Committed\"" + suffix + "}";
}
std::string document(std::string files, std::string extra = {})
{
    return "{\"schema\":\"goden.tag-operation.v2\",\"operationId\":\"test\",\"files\":[" + files + "]" + extra + "}";
}
int main()
{
    try {
        const auto original = std::string("\xef\xbb\xbf") + " \n" + document(file(",\"detail\":\"old\\\"{[]}\""),
            ",\"future\":18446744073709551615,\"items\":[{\"id\":9007199254740993}],\"nested\":{\"x\":[true,null,-1.2300e+04]}") + "\r\n";
        auto parsed = TagRecoveryJournal::parse(original);
        require(parsed.has_value(), "BOM/whitespace and nested literals accepted");
        require(parsed->render() == original, "Untouched document is byte exact");
        require(parsed->matchesOperation("test") && !parsed->matchesOperation("other"), "Operation identity checked");
        require(parsed->matchesFile(0,"track","backup",std::string(64,'a'),std::string(64,'b')), "Exact source record matched");
        require(!parsed->matchesFile(0,"other","backup",std::string(64,'a'),std::string(64,'b')), "Different source cannot be patched");
        require(!parsed->markRestored(1,"missing"), "Out of bounds file rejected");
        require(parsed->markRestored(0,"done\n\"quotes\" \\ slash"), "Detail safely staged");
        const auto output = parsed->render();
        require(output && output->find("18446744073709551615") != std::string::npos &&
            output->find("9007199254740993") != std::string::npos && output->find("-1.2300e+04") != std::string::npos,
            "Unknown integer precision and number spelling preserved");
        require(json::parse_strict(*output)["files"][0]["detail"].as_string() == "done\n\"quotes\" \\ slash", "Patched string valid JSON");
        auto missing = TagRecoveryJournal::parse(document(file()));
        require(missing && missing->markRestored(0,"inserted"), "Missing optional detail supported");
        require(json::parse_strict(*missing->render())["files"][0]["detail"].as_string() == "inserted", "Detail insertion retains object syntax");
        require(!TagRecoveryJournal::parse(document(file()) + "junk"), "Trailing garbage rejected");
        require(!TagRecoveryJournal::parse(document(file(),",\"files\":[]")), "Duplicate fields rejected");
        require(!TagRecoveryJournal::parse("{\"schema\":false}"), "Invalid schema rejected");
        require(!TagRecoveryJournal::parse(std::string(TagRecoveryJournal::maxBytes + 1,' ')), "Oversized input rejected before parsing");
        auto maximum = document(file());maximum.resize(TagRecoveryJournal::maxBytes,' ');
        auto large = TagRecoveryJournal::parse(maximum);
        require(large.has_value(), "Existing 4 MiB journal limit retained");
        require(large->markRestored(0,"new detail"), "Max-size patch planned");
        require(!large->render(), "Output may not grow beyond recovery budget");
        require(!large->markRestored(0,std::string(4097,'x')), "Detail bounded");

        const auto first = file(",\"detail\":\"old\"");
        const auto middle = "{\"outcome\":\"Restored\",\"detail\":\"untouched\"}";
        const auto last = file();
        auto multi = TagRecoveryJournal::parse(document(first + "," + middle + "," + last));
        require(multi && multi->markRestored(2,"two") && multi->markRestored(0,"one"), "Multiple files patched out of order");
        auto firstExpected = first;
        firstExpected.replace(firstExpected.find("\"Committed\""),11,"\"Restored\"");
        firstExpected.replace(firstExpected.find("\"old\""),5,"\"one\"");
        auto lastExpected = "{\"detail\":\"two\"," + last.substr(1);
        lastExpected.replace(lastExpected.find("\"Committed\""),11,"\"Restored\"");
        require(multi->render() == document(firstExpected + "," + middle + "," + lastExpected), "Sorted patches compose without changing intervening rows");

        // Vary file offsets, escaped field names and nested values. The output
        // expectation is formed without the production span walker.
        for (int n = 0; n < 256; ++n) {
            const auto before = n % 3 == 0 ? "null,{\"outcome\":\"Restored\",\"detail\":\"unrelated\"}," : "";
            auto target = file(",\"detail\":\"old\",\"unknown\":{\"str\":\"\\\"[]{}\\\\\",\"num\":1.000e-02}");
            if (n % 2 == 0) target.replace(target.find("outcome"), 7, "outc\\u006fme");
            auto raw = document(before + target + ",false", ",\"revision\":" + std::to_string(n));
            if (n % 4 == 0) raw.replace(raw.find("files"), 5, "fi\\u006ces");
            const auto index = n % 3 == 0 ? 2U : 0U;
            auto journal = TagRecoveryJournal::parse(raw);
            require(journal && journal->matchesFile(index,"track","backup",std::string(64,'a'),std::string(64,'b')), "Escaped keys retain physical index");
            require(journal->markRestored(index,"verified"), "Selected file patched");
            auto expected = raw;
            expected.replace(expected.find("\"Committed\""),11,"\"Restored\"");
            expected.replace(expected.find("\"old\""),5,"\"verified\"");
            require(journal->render() == expected, "Only designated value spans differ");
            require(journal->markRestored(index,"verified") && journal->render() == expected, "Repeated consumption idempotent");
        }
        std::cout << "PASS recovery-journal checks=" << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
