// Copyright 2016 Citra Emulator Project
// Licensed under GPLv2 or any later version
// Refer to the misc/licenses/gplv2.txt file included.

#include <catch2/catch_test_macros.hpp>
#include "common/file_util.h"
#include "core/file_sys/path_parser.h"
#include "core/file_sys/errors.h"
#include "core/file_sys/savedata_archive.h"

namespace FileSys {

TEST_CASE("PathParser", "[core][file_sys]") {
    REQUIRE(!PathParser(Path(std::vector<u8>{})).IsValid());
    REQUIRE(!PathParser(Path("a")).IsValid());
    REQUIRE(!PathParser(Path("/|")).IsValid());
    REQUIRE(PathParser(Path("/a")).IsValid());
    REQUIRE(!PathParser(Path("/a/b/../../c/../../d")).IsValid());
    REQUIRE(PathParser(Path("/a/b/../c/../../d")).IsValid());
    REQUIRE(PathParser(Path("/")).IsRootDirectory());
    REQUIRE(!PathParser(Path("/a")).IsRootDirectory());
    REQUIRE(PathParser(Path("/a/..")).IsRootDirectory());
}

TEST_CASE("PathParser - Host file system", "[core][file_sys]") {
    std::string test_dir = "./test";
    FileUtil::CreateDir(test_dir);
    FileUtil::CreateDir(test_dir + "/z");
    FileUtil::CreateEmptyFile(test_dir + "/a");

    REQUIRE(PathParser(Path("/a")).GetHostStatus(test_dir) == PathParser::FileFound);
    REQUIRE(PathParser(Path("/b")).GetHostStatus(test_dir) == PathParser::NotFound);
    REQUIRE(PathParser(Path("/z")).GetHostStatus(test_dir) == PathParser::DirectoryFound);
    REQUIRE(PathParser(Path("/a/c")).GetHostStatus(test_dir) == PathParser::FileInPath);
    REQUIRE(PathParser(Path("/b/c")).GetHostStatus(test_dir) == PathParser::PathNotFound);

    FileUtil::DeleteDirRecursively(test_dir);
}

TEST_CASE("Save archive path limits", "[core][file_sys][accuracy]") {
    CHECK(PathParser(Path("/1234567890123456"), 16, 248).IsValid());
    CHECK_FALSE(PathParser(Path("/12345678901234567"), 16, 248).IsValid());
    CHECK_FALSE(PathParser(Path("/12345678901234567/a"), 16, 248).IsValid());
    CHECK(PathParser(Path("/world_resource_packs.json")).IsValid());
    CHECK_FALSE(PathParser(Path("/world_resource_packs.json"), 16, 248).IsValid());
    CHECK(PathParser(Path("/packcfg.json.new"), 16, 248).IsValid());
    const std::string japanese = "/" + std::string(16, 'a') + "/\xE6\x97\xA5";
    CHECK(PathParser(Path(japanese), 16, 248).IsValid());
    std::string full_path;
    for (int i = 0; i < 14; i++) full_path += "/1234567890123456";
    full_path += "/123456789";
    REQUIRE(full_path.size() == 248);
    CHECK(PathParser(Path(full_path), 16, 248).IsValid());
    CHECK_FALSE(PathParser(Path(full_path + "0"), 16, 248).IsValid());
    CHECK(PathParser(Path(full_path + "01234"), 16, 253).IsValid());
    CHECK_FALSE(PathParser(Path(full_path + "012345"), 16, 253).IsValid());

    SaveDataArchive archive("./nonexistent-accuracy-save/");
    const Path invalid("/world_resource_packs.json");
    const Path valid("/packs.json");
    Mode mode{};
    mode.read_flag.Assign(1);
    CHECK(archive.OpenFile(invalid, mode, 0).Code() == ResultInvalidPath);
    CHECK(archive.OpenDirectory(invalid).Code() == ResultInvalidPath);
    CHECK(archive.CreateFile(invalid, 1, 0) == ResultInvalidPath);
    CHECK(archive.DeleteFile(invalid) == ResultInvalidPath);
    CHECK(archive.RenameFile(invalid, valid) == ResultInvalidPath);
    CHECK(archive.RenameFile(valid, invalid) == ResultInvalidPath);
    CHECK(archive.CreateDirectory(invalid, 0) == ResultInvalidPath);
    CHECK(archive.DeleteDirectory(invalid) == ResultInvalidPath);
    CHECK(archive.DeleteDirectoryRecursively(invalid) == ResultInvalidPath);
    CHECK(archive.RenameDirectory(invalid, valid) == ResultInvalidPath);
    CHECK(archive.RenameDirectory(valid, invalid) == ResultInvalidPath);
}

} // namespace FileSys
