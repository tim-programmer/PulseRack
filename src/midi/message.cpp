#include "midi/message.h"

namespace rack::midi
{

bool decode(std::span<const uint8_t> b, Msg *out)
{
    if (b.empty() || !(b[0] & 0x80))
        return false;
    const uint8_t status = b[0];
    if (status >= 0xF0)
    {
        Type t;
        switch (status)
        {
        case 0xF8:
            t = Type::Clock;
            break;
        case 0xFA:
            t = Type::Start;
            break;
        case 0xFB:
            t = Type::Continue;
            break;
        case 0xFC:
            t = Type::Stop;
            break;
        default:
            return false; // SysEx, time code, song position, tune request, sensing, reset
        }
        Msg m;
        m.type = t;
        *out = m;
        return true;
    }
    // channel messages: program change and channel pressure have one data byte, the rest two
    const uint8_t kind = status & 0xF0;
    const size_t  need = (kind == 0xC0 || kind == 0xD0) ? 2 : 3;
    if (b.size() != need)
        return false;
    for (size_t i = 1; i < need; i++)
        if (b[i] & 0x80)
            return false;
    Msg m;
    m.channel = status & 0x0F;
    const uint8_t d1 = b[1], d2 = need == 3 ? b[2] : 0;
    switch (kind)
    {
    case 0x80:
        m.type = Type::NoteOff, m.data = d1, m.value = d2;
        break;
    case 0x90:
        m.type = d2 ? Type::NoteOn : Type::NoteOff, m.data = d1, m.value = d2;
        break;
    case 0xA0:
        m.type = Type::KeyPressure, m.data = d1, m.value = d2;
        break;
    case 0xB0:
        m.type = Type::Control, m.data = d1, m.value = d2;
        break;
    case 0xC0:
        m.type = Type::Program, m.data = d1;
        break;
    case 0xD0:
        m.type = Type::ChannelPressure, m.value = d1;
        break;
    case 0xE0:
        m.type = Type::PitchBend, m.value = uint16_t(d1 | (d2 << 7));
        break;
    default:
        return false;
    }
    *out = m;
    return true;
}

} // namespace rack::midi
