// Unit tests for PtyConsole (src/helpers/PtyConsole.cpp).
//
// NOTE: intentionally NOT wired into [env:native] in the tracked platformio.ini
// (adding sources there can disturb the MeshCore CI pipelines). To run locally,
// add a gitignored platformio.local.ini that redefines [env:native] with
//   -D MESHCORE_HOST_TEST  and  +<../src/helpers/PtyConsole.cpp>
// then: pio test -e native -f test_console  (run the built binary directly if the
// pio test runner hits a tty/SIGHUP artifact in a non-interactive shell).

#include <gtest/gtest.h>

#include "helpers/PtyConsole.h"

#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string>

namespace {

std::string unique_link(const char *tag) {
    return std::string("/tmp/meshcore-test-pty-") + tag + "-" +
           std::to_string(getpid());
}

// Open the PTY slave (via the console's published path) as a raw serial client.
int open_client(const char *path) {
    int fd = ::open(path, O_RDWR | O_NOCTTY);
    EXPECT_GE(fd, 0);
    if (fd >= 0) {
        termios t{};
        if (tcgetattr(fd, &t) == 0) { cfmakeraw(&t); tcsetattr(fd, TCSANOW, &t); }
    }
    return fd;
}

int wait_available(PtyConsole &c, int want) {
    int avail = 0;
    for (int i = 0; i < 200 && avail < want; i++) {
        avail = c.available();
        if (avail < want) usleep(1000);
    }
    return avail;
}

// Read like the repeater's loop(): only read() while available() reports bytes.
std::string read_like_consumer(PtyConsole &c, char terminator) {
    std::string out;
    for (int i = 0; i < 400 && out.find(terminator) == std::string::npos; i++) {
        while (c.available() > 0) {
            int ch = c.read();
            if (ch < 0) break;
            out += (char)ch;
        }
        if (out.find(terminator) == std::string::npos) usleep(1000);
    }
    return out;
}

}  // namespace

TEST(PtyConsole, DeliversClientBytesToRead) {
    PtyConsole c;
    ASSERT_TRUE(c.begin(unique_link("read").c_str()));
    int client = open_client(c.path());
    ASSERT_GE(client, 0);
    ASSERT_EQ(::write(client, "hi", 2), 2);

    EXPECT_GE(wait_available(c, 2), 2);
    EXPECT_EQ(c.read(), 'h');
    EXPECT_EQ(c.read(), 'i');

    ::close(client);
    c.end();
}

TEST(PtyConsole, MapsNewlineToCarriageReturn) {
    // The CLI terminates a command on '\r'; tools send '\n'. Map 1:1 so it works.
    PtyConsole c;
    ASSERT_TRUE(c.begin(unique_link("nl").c_str()));
    int client = open_client(c.path());
    ASSERT_GE(client, 0);
    ASSERT_EQ(::write(client, "hi\n", 3), 3);
    EXPECT_GE(wait_available(c, 3), 3);

    EXPECT_EQ(c.read(), 'h');
    EXPECT_EQ(c.read(), 'i');
    EXPECT_EQ(c.read(), '\r');

    ::close(client);
    c.end();
}

TEST(PtyConsole, WriteReachesConnectedClient) {
    PtyConsole c;
    ASSERT_TRUE(c.begin(unique_link("write").c_str()));
    int client = open_client(c.path());
    ASSERT_GE(client, 0);

    c.write((uint8_t)'O');
    c.write((uint8_t)'K');

    char buf[2] = {0, 0};
    ssize_t got = 0;
    for (int i = 0; i < 200 && got < 2; i++) {
        ssize_t n = ::read(client, buf + got, 2 - got);
        if (n > 0) got += n; else usleep(1000);
    }
    EXPECT_EQ(got, 2);
    EXPECT_EQ(buf[0], 'O');
    EXPECT_EQ(buf[1], 'K');

    ::close(client);
    c.end();
}

TEST(PtyConsole, PeekDoesNotConsume) {
    PtyConsole c;
    ASSERT_TRUE(c.begin(unique_link("peek").c_str()));
    int client = open_client(c.path());
    ASSERT_GE(client, 0);
    ASSERT_EQ(::write(client, "Z", 1), 1);
    EXPECT_GE(wait_available(c, 1), 1);

    EXPECT_EQ(c.peek(), 'Z');
    EXPECT_EQ(c.peek(), 'Z');
    EXPECT_EQ(c.read(), 'Z');
    EXPECT_EQ(c.read(), -1);

    ::close(client);
    c.end();
}

TEST(PtyConsole, NoInputBeforeClientWrites) {
    PtyConsole c;
    ASSERT_TRUE(c.begin(unique_link("noinput").c_str()));

    EXPECT_EQ(c.available(), 0);
    EXPECT_EQ(c.read(), -1);
    EXPECT_EQ(c.peek(), -1);

    c.end();
}

TEST(PtyConsole, ServesNewClientAfterCloseConsumerLoopShape) {
    // The master persists across client open/close; the consumer only read()s
    // when available()>0.
    PtyConsole c;
    ASSERT_TRUE(c.begin(unique_link("reconnect").c_str()));

    int c1 = open_client(c.path());
    ASSERT_GE(c1, 0);
    ASSERT_EQ(::write(c1, "a\n", 2), 2);
    EXPECT_EQ(read_like_consumer(c, '\r'), "a\r");
    ::close(c1);

    for (int i = 0; i < 20; i++) { c.available(); usleep(1000); }

    int c2 = open_client(c.path());
    ASSERT_GE(c2, 0);
    ASSERT_EQ(::write(c2, "b\n", 2), 2);
    EXPECT_EQ(read_like_consumer(c, '\r'), "b\r");

    ::close(c2);
    c.end();
}

TEST(PtyConsole, WriteAfterClientCloseDoesNotCrash) {
    PtyConsole c;
    ASSERT_TRUE(c.begin(unique_link("wclose").c_str()));
    int client = open_client(c.path());
    ASSERT_GE(client, 0);
    c.write((uint8_t)'x');
    ::close(client);

    usleep(20000);
    for (int i = 0; i < 200; i++) c.write((uint8_t)'y');
    SUCCEED();  // no signal / crash

    c.end();
}

TEST(PtyConsole, SlaveIsOwnerOnlyCharDevAndSymlinkUnlinkedOnEnd) {
    std::string link = unique_link("perms");
    PtyConsole c;
    ASSERT_TRUE(c.begin(link.c_str()));

    struct stat st{};
    ASSERT_EQ(::stat(c.path(), &st), 0);   // follows the symlink to the pts device
    EXPECT_TRUE(S_ISCHR(st.st_mode));
    EXPECT_EQ(st.st_mode & 0777, 0600u);   // owner-only: the access gate

    c.end();
    struct stat ls{};
    EXPECT_NE(::lstat(link.c_str(), &ls), 0);  // published symlink removed
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
