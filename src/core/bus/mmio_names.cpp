#include "core/bus/mmio_names.hpp"

#include <algorithm>
#include <array>
#include <iterator>

namespace core {

namespace {

struct NamedRange {
    u32 first;
    u32 last;  // inclusive
    std::string_view name;
};

// Sorted by address. Single registers use first == last.
constexpr std::array kNames = {
    // MADAM (0x03300000)
    NamedRange{0x03300000, 0x03300000, "MADAM Revision"},
    NamedRange{0x03300004, 0x03300004, "MADAM MSysBits"},
    NamedRange{0x03300008, 0x03300008, "MADAM MCTL"},
    NamedRange{0x03300028, 0x03300028, "MADAM StatBits"},
    NamedRange{0x03300100, 0x03300100, "MADAM SPRSTRT"},
    NamedRange{0x03300104, 0x03300104, "MADAM SPRSTOP"},
    NamedRange{0x03300108, 0x03300108, "MADAM SPRCNTU"},
    NamedRange{0x0330010C, 0x0330010C, "MADAM SPRPAUS"},
    NamedRange{0x03300110, 0x03300110, "MADAM CEControl"},
    NamedRange{0x03300130, 0x0330013C, "MADAM REGCTL0-3"},
    NamedRange{0x03300140, 0x0330015C, "MADAM cel XY/DXY"},
    NamedRange{0x03300400, 0x0330053F, "MADAM CLIO FIFO"},
    NamedRange{0x03300570, 0x03300570, "MADAM PlayerDMA Dest"},
    NamedRange{0x03300574, 0x03300574, "MADAM PlayerDMA Len"},
    NamedRange{0x03300578, 0x03300578, "MADAM PlayerDMA Out"},
    NamedRange{0x03300580, 0x03300580, "MADAM VDL Head"},
    NamedRange{0x033005A0, 0x033005BC, "MADAM cel engine DMA"},
    NamedRange{0x03300600, 0x033006FF, "MADAM matrix engine"},
    // CLIO (0x03400000)
    NamedRange{0x03400000, 0x03400000, "CLIO ClioRev"},
    NamedRange{0x03400004, 0x03400004, "CLIO CSysBits"},
    NamedRange{0x03400008, 0x03400008, "CLIO VInt0"},
    NamedRange{0x0340000C, 0x0340000C, "CLIO VInt1"},
    NamedRange{0x03400020, 0x03400020, "CLIO AudioIn"},
    NamedRange{0x03400024, 0x03400024, "CLIO AudioOut"},
    NamedRange{0x03400028, 0x03400028, "CLIO CStatBits"},
    NamedRange{0x0340002C, 0x0340002C, "CLIO WatchDog"},
    NamedRange{0x03400030, 0x03400030, "CLIO HCnt"},
    NamedRange{0x03400034, 0x03400034, "CLIO VCnt"},
    NamedRange{0x03400038, 0x03400038, "CLIO RandSeed"},
    NamedRange{0x0340003C, 0x0340003C, "CLIO RandSample"},
    NamedRange{0x03400040, 0x03400040, "CLIO Int0 Set"},
    NamedRange{0x03400044, 0x03400044, "CLIO Int0 Clr"},
    NamedRange{0x03400048, 0x03400048, "CLIO Int0 SetEn"},
    NamedRange{0x0340004C, 0x0340004C, "CLIO Int0 ClrEn"},
    NamedRange{0x03400050, 0x03400050, "CLIO SetMode"},
    NamedRange{0x03400054, 0x03400054, "CLIO ClrMode"},
    NamedRange{0x03400058, 0x03400058, "CLIO BadBits"},
    NamedRange{0x03400060, 0x03400060, "CLIO Int1 Set"},
    NamedRange{0x03400064, 0x03400064, "CLIO Int1 Clr"},
    NamedRange{0x03400068, 0x03400068, "CLIO Int1 SetEn"},
    NamedRange{0x0340006C, 0x0340006C, "CLIO Int1 ClrEn"},
    NamedRange{0x03400080, 0x03400080, "CLIO HDelay"},
    NamedRange{0x03400084, 0x03400084, "CLIO ADBIOBits"},
    NamedRange{0x03400088, 0x03400088, "CLIO ADBCTLBits"},
    NamedRange{0x03400100, 0x0340017F, "CLIO Timers"},
    NamedRange{0x03400200, 0x0340020F, "CLIO TimerControl"},
    NamedRange{0x03400220, 0x03400220, "CLIO TimerSlack"},
    NamedRange{0x03400300, 0x03400300, "CLIO FifoInit"},
    NamedRange{0x03400304, 0x03400304, "CLIO SetDMAEnable"},
    NamedRange{0x03400308, 0x03400308, "CLIO ClrDMAEnable"},
    NamedRange{0x03400380, 0x034003FF, "CLIO FIFO status"},
    NamedRange{0x03400400, 0x03400BFF, "CLIO Expansion bus (XBUS)"},
    NamedRange{0x034017D0, 0x034017D4, "CLIO DSPP semaphore"},
    NamedRange{0x034017E0, 0x034017FF, "CLIO DSPP control"},
    NamedRange{0x03401800, 0x03403FFF, "CLIO DSPP memory"},
};

}  // namespace

std::string_view mmio_register_name(u32 address) {
    address &= ~3u;
    const auto it = std::ranges::upper_bound(kNames, address, {}, &NamedRange::first);
    if (it == kNames.begin()) return {};
    const NamedRange& range = *std::prev(it);
    return address <= range.last ? range.name : std::string_view{};
}

}  // namespace core
