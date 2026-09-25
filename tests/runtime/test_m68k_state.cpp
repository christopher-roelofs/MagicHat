/*
 * test_m68k_state.cpp — saving a 68k machine and getting the same one back.
 *
 * The question a state file has to answer is not "did it write something"
 * but "is the machine that comes back the machine that went in", and the
 * only honest way to ask it is to let both keep running and see whether they
 * stay together. So this runs one machine to a point, saves, runs on; then
 * restores into a second machine and runs it the same distance; then saves
 * both and compares the files byte for byte.
 *
 * That comparison is deliberately stricter than anything the interface
 * exposes. It covers the registers no accessor reports, the second bank of
 * memory nothing can read back, the device registers, and the battery store
 * -- and it fails on anything I forgot to carry across, which is the whole
 * risk with a format written by hand.
 *
 * The fixture writes into the framebuffer on purpose. Low memory is ROM out
 * of reset -- a 68300's boot chip select answers the whole address space
 * until software gives it a real window -- so a restore that forgets to put
 * the overlay back where it was would quietly send every one of those writes
 * to ROM, where they are ignored. The picture is how that shows up.
 *
 * And it runs the whole thing four times, once for each pairing of the two
 * engines. Nothing in the format is supposed to know which one is running --
 * the block engine hands the processor back to the reference core before
 * mrc_m68k_run returns, so the reference core is the state of record at every
 * point a caller could save -- but that is an argument, and saving on one
 * engine and restoring on the other is the measurement.
 *
 * Both machines run the second leg on the *loading* engine, which is what
 * makes the comparison exact. Two engines running the same instructions do
 * not leave the same machine behind in every respect: the reference core
 * keeps a cycle clock and the block engine counts instructions, so their
 * idea of elapsed cycles diverges by design -- that is the very reason the
 * Envoy declines the block engine, since its battery RAM is read off that
 * count. Comparing a machine that ran one engine against a machine that ran
 * the other would be measuring that, not the state file.
 */
#include "machines/pic2000/machine68k.h"
#include "machines/pic2000/board.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <string>
#include <vector>

static int failures;
#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "line %d: %s\n", __LINE__, #x); failures++; } } while (0)

namespace {

std::vector<unsigned char> rom_image(unsigned flavour)
{
    std::vector<unsigned char> rom(PIC2000_ROM_SIZE);
    auto word = [&](unsigned a, unsigned v) {
        rom[a] = (unsigned char)(v >> 8);
        rom[a + 1] = (unsigned char)v;
    };
    auto code = [&](unsigned at, std::initializer_list<unsigned> words) {
        for (unsigned v : words) { word(at, v); at += 2; }
    };

    /* Reset vector pair: stack at 1 MB, entry at ROM + 0x300. */
    word(0, 0x0010); word(2, 0x0000);
    word(4, 0x0e00); word(6, 0x0300);

    /*
     * Give CS0 the window it is already running from. That is the moment the
     * boot overlay retires and address 0 becomes DRAM, and it is the one
     * piece of bus state a snapshot has to put back by hand.
     */
    code(0x300, { 0x23fc, 0x0e00, 0x00f9, 0x3c00, 0x0044,
                  /* Turn the panel on. Retained framebuffer bytes are not
                   * visible until the guest enables it, so without this the
                   * picture below would be the powered-off one either way. */
                  0x33fc, 0x0010, 0x2100, 0x0040,
                  0x4ef9, 0x0e00, 0x0400 });                 /* jmp $0E000400 */

    /*
     * Then a loop that leaves a mark in both banks of memory and in the
     * registers: a1 walks the framebuffer, a2 walks the second bank, and d0
     * is stirred so that two machines a few instructions apart look nothing
     * like each other.
     *
     * `flavour` only changes the constant it starts from, which is enough to
     * make a second ROM that a state from the first must refuse to load into.
     */
    code(0x400, { 0x43f9, 0x0000, 0x2800,        /* lea    $00002800,a1   */
                  0x45f9, 0x0400, 0x0000,        /* lea    $04000000,a2   */
                  0x203c, 0x0000, flavour });    /* move.l #flavour,d0    */
    code(0x412, { 0x5280,                        /* loop: addq.l #1,d0    */
                  0xe598,                        /*       rol.l  #2,d0    */
                  0x22c0,                        /*       move.l d0,(a1)+ */
                  0x24c0,                        /*       move.l d0,(a2)+ */
                  0xb3fc, 0x0000, 0x3800,        /*       cmpa.l #$3800,a1*/
                  0x66f0,                        /*       bne.s  loop     */
                  0x43f9, 0x0000, 0x2800,        /*       lea $2800,a1    */
                  0x45f9, 0x0400, 0x0000,        /*       lea $04000000,a2*/
                  0x60e2 });                     /*       bra.s  loop     */
    return rom;
}

void write_file(const std::filesystem::path &p,
                const std::vector<unsigned char> &bytes)
{
    std::ofstream f(p, std::ios::binary);
    f.write((const char *)bytes.data(), (std::streamsize)bytes.size());
}

std::vector<unsigned char> read_file(const std::filesystem::path &p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<unsigned char>(std::istreambuf_iterator<char>(f),
                                      std::istreambuf_iterator<char>());
}

std::vector<uint8_t> screen(m68k_machine *m)
{
    std::vector<uint8_t> px(PIC2000_SCREEN_W * PIC2000_SCREEN_H);
    mrc_m68k_lcd(m, px.data());
    return px;
}

bool blank(const std::vector<uint8_t> &px)
{
    for (uint8_t v : px) if (v) return false;
    return true;
}

/*
 * One machine saved and another restored into it, with the engine each one
 * runs named separately.
 */
bool round_trip(const std::filesystem::path &dir, const std::filesystem::path &rom,
               FILE *log, const char *save_engine, const char *load_engine)
{
    const std::string tag = std::string(save_engine) + "-" + load_engine;
    const auto at_t1 = (dir / (tag + "-t1.state")).string();
    const auto from_a = (dir / (tag + "-a.state")).string();
    const auto from_b = (dir / (tag + "-b.state")).string();

    /* The two legs of the run. The first is long enough to retire the boot
     * overlay and get well into the loop; the second is not a whole number of
     * passes, so a machine restored one pass out of step would not line up. */
    const uint64_t leg1 = 20000, leg2 = 7331;

    m68k_machine *a = mrc_m68k_new(rom.string().c_str(), 4, log);
    CHECK(a != nullptr);
    if (!a) return false;
    CHECK(mrc_m68k_set_engine(a, save_engine));

    CHECK(mrc_m68k_run(a, leg1) == leg1);
    const uint64_t insns_t1 = mrc_m68k_insns(a);
    const auto picture_t1 = screen(a);
    CHECK(!blank(picture_t1));      /* the overlay really did retire */
    CHECK(mrc_m68k_save_state(a, at_t1.c_str()));

    /* From here both machines run the engine the state is being restored
     * onto, so what is left is the state file and nothing else. */
    CHECK(mrc_m68k_set_engine(a, load_engine));
    CHECK(mrc_m68k_run(a, leg2) == leg2);
    const auto picture_t2 = screen(a);
    CHECK(picture_t2 != picture_t1);
    const uint64_t insns_t2 = mrc_m68k_insns(a);
    CHECK(mrc_m68k_save_state(a, from_a.c_str()));

    /* A second machine, built from the same ROM and nowhere near the same
     * place, told to become the first one. */
    m68k_machine *b = mrc_m68k_new(rom.string().c_str(), 4, log);
    CHECK(b != nullptr);
    if (!b) { mrc_m68k_free(a); return false; }
    CHECK(mrc_m68k_set_engine(b, load_engine));
    CHECK(mrc_m68k_run(b, 977) == 977);        /* somewhere else entirely */
    CHECK(screen(b) != picture_t1);
    CHECK(mrc_m68k_load_state(b, at_t1.c_str()));
    CHECK(screen(b) == picture_t1);
    CHECK(mrc_m68k_insns(b) == insns_t1);

    CHECK(mrc_m68k_run(b, leg2) == leg2);
    CHECK(screen(b) == picture_t2);
    CHECK(mrc_m68k_insns(b) == insns_t2);
    CHECK(mrc_m68k_save_state(b, from_b.c_str()));

    /*
     * The real check. Everything the interface can show has already agreed;
     * this is everything it cannot -- the second bank of memory, the
     * registers with no accessor, the device state, the battery store.
     */
    const auto bytes_a = read_file(from_a), bytes_b = read_file(from_b);
    CHECK(!bytes_a.empty());
    CHECK(bytes_a.size() == bytes_b.size());
    if (bytes_a.size() == bytes_b.size()) {
        size_t differ = 0, first = 0;
        for (size_t i = 0; i < bytes_a.size(); i++)
            if (bytes_a[i] != bytes_b[i]) { if (!differ) first = i; differ++; }
        if (differ)
            std::fprintf(stderr, "%s: restored machine differs in %zu of %zu "
                         "bytes, first at offset %zu\n", tag.c_str(), differ,
                         bytes_a.size(), first);
        CHECK(differ == 0);
    }

    /*
     * A truncated file is refused, and refusing leaves the machine alone.
     * Reading straight into the machine would have left it half one state and
     * half another, which is worse than not loading at all.
     */
    const auto cut = (dir / (tag + "-cut.state")).string();
    {
        auto bytes = read_file(at_t1);
        bytes.resize(bytes.size() / 2);
        write_file(cut, bytes);
    }
    const auto before = screen(b);
    const uint64_t insns_before = mrc_m68k_insns(b);
    CHECK(!mrc_m68k_load_state(b, cut.c_str()));
    CHECK(screen(b) == before);
    CHECK(mrc_m68k_insns(b) == insns_before);

    CHECK(!mrc_m68k_load_state(b, (dir / "absent.state").string().c_str()));

    /* And a state saved after a load is still a state: restoring is not a
     * one-way door, which is what switching between devices will need. */
    const auto again = (dir / (tag + "-again.state")).string();
    CHECK(mrc_m68k_save_state(b, again.c_str()));
    CHECK(mrc_m68k_load_state(b, again.c_str()));
    CHECK(screen(b) == before);

    mrc_m68k_free(a);
    mrc_m68k_free(b);
    return true;
}

}  /* namespace */

int main()
{
    auto dir = std::filesystem::temp_directory_path() /
        ("mrc-m68k-state-" +
         std::to_string(std::chrono::steady_clock::now()
                        .time_since_epoch().count()));
    std::filesystem::create_directory(dir);

    const auto rom = dir / "fixture.rom";
    const auto other = dir / "other.rom";
    write_file(rom, rom_image(0x1111));
    write_file(other, rom_image(0x2222));

    FILE *log = std::tmpfile();

    /* Save and restore across the single-step and block execution paths. */
    std::vector<const char *> engines = { "interpreter", "blocks" };
    for (const char *saver : engines)
        for (const char *loader : engines)
            CHECK(round_trip(dir, rom, log, saver, loader));

    /*
     * The option key is part of the machine, not part of the window.
     *
     * It is a line the guest samples rather than an edge it is told about, so
     * it is held rather than pressed -- and that means a device can be put
     * down with it down. The window used to keep its own idea of whether it
     * was held, which a restore silently contradicted: the guest saw the key
     * down and the button showed it up. Asking the machine is the fix, and
     * this is the property that makes asking work.
     */
    {
        const auto held = (dir / "held.state").string();
        m68k_machine *k = mrc_m68k_new(rom.string().c_str(), 4, log);
        CHECK(k != nullptr);
        if (k) {
            CHECK(mrc_m68k_run(k, 20000) == 20000);
            CHECK(!mrc_m68k_option_held(k));
            mrc_m68k_set_option(k, true);
            CHECK(mrc_m68k_option_held(k));
            CHECK(mrc_m68k_save_state(k, held.c_str()));
            mrc_m68k_free(k);
        }
        m68k_machine *back = mrc_m68k_new(rom.string().c_str(), 4, log);
        CHECK(back != nullptr);
        if (back) {
            CHECK(!mrc_m68k_option_held(back));   /* fresh: not held */
            CHECK(mrc_m68k_load_state(back, held.c_str()));
            CHECK(mrc_m68k_option_held(back));    /* restored: still held */
            mrc_m68k_set_option(back, false);     /* and it still releases */
            CHECK(!mrc_m68k_option_held(back));
            mrc_m68k_free(back);
        }
    }

    /*
     * A state belongs to the machine it came from. Both of these would
     * otherwise produce a machine that looks alive and is not, so both have
     * to be refused: a different ROM, and a different amount of memory.
     */
    const auto a_state =
        (dir / (std::string(engines.front()) + "-" + engines.front() +
                "-t1.state")).string();
    {
        const auto serial_state = (dir / "serial-a.state").string();
        auto *source = mrc_m68k_new(rom.string().c_str(), 4, log);
        auto *restored = mrc_m68k_new(rom.string().c_str(), 4, log);
        CHECK(source && restored);
        if (source && restored) {
            CHECK(mrc_m68k_open_serial_a(source));
            auto &a = source->duart.a;
            a.rx_enabled = a.tx_enabled = a.tx_busy = true;
            a.rx[0] = 0x7e; a.rx[1] = 0x7d; a.rx[2] = 0xff;
            a.rx_at = 2; a.rx_len = 3; a.tx_byte = 'T';
            a.tx_done_at = 12345; a.rx_next_at = 67890;
            CHECK(mrc_m68k_save_state(source, serial_state.c_str()));
            CHECK(mrc_m68k_load_state(restored, serial_state.c_str()));
            const auto &b = restored->duart.a;
            CHECK(b.enabled && b.rx_enabled && b.tx_enabled && b.tx_busy);
            CHECK(b.rx_at == 2 && b.rx_len == 3 && b.tx_byte == 'T');
            CHECK(!std::memcmp(a.rx, b.rx, 3));
            CHECK(b.tx_done_at == 12345 && b.rx_next_at == 67890);
            CHECK(restored->duart.link_a.fd == -1 && !restored->duart.link_a.peer);
            CHECK(!restored->magicbus.connected);
            // Bad FIFO length must fail before changing any live state.
            {
                std::fstream file(serial_state, std::ios::in | std::ios::out | std::ios::binary);
                file.seekp(-12, std::ios::end); // A trailer byte 20: rx_len
                file.put(4);
            }
            CHECK(!mrc_m68k_load_state(restored, serial_state.c_str()));
            CHECK(restored->duart.a.rx_len == 3);
            CHECK(mrc_m68k_save_state(source, serial_state.c_str()));
            std::filesystem::resize_file(serial_state, std::filesystem::file_size(serial_state) - 1);
            CHECK(!mrc_m68k_load_state(restored, serial_state.c_str()));
            CHECK(restored->duart.a.rx_len == 3);
            CHECK(mrc_m68k_load_state(restored, a_state.c_str()));
            CHECK(!restored->duart.a.enabled && !restored->duart.a.rx_len);
        }
        mrc_m68k_free(source);
        mrc_m68k_free(restored);
    }
    m68k_machine *wrong_rom = mrc_m68k_new(other.string().c_str(), 4, log);
    CHECK(wrong_rom != nullptr);
    if (wrong_rom) {
        CHECK(!mrc_m68k_load_state(wrong_rom, a_state.c_str()));
        mrc_m68k_free(wrong_rom);
    }
    m68k_machine *wrong_ram = mrc_m68k_new(rom.string().c_str(), 8, log);
    CHECK(wrong_ram != nullptr);
    if (wrong_ram) {
        CHECK(!mrc_m68k_load_state(wrong_ram, a_state.c_str()));
        mrc_m68k_free(wrong_ram);
    }

    std::fclose(log);
    /* MRC_KEEP=1 leaves the state files behind: when this fails it fails by
     * naming a byte offset, and the files are what turn that into a field. */
    if (std::getenv("MRC_KEEP"))
        std::fprintf(stderr, "states kept in %s\n", dir.string().c_str());
    else
        std::filesystem::remove_all(dir);

    if (failures) std::fprintf(stderr, "%d state checks failed\n", failures);
    else std::printf("all 68k state checks passed, both engines either way\n");
    return failures ? 1 : 0;
}
