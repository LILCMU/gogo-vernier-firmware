#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Callback signature; 'root' valid only during the call
using OnMsgPackMessage = void (*)(JsonVariantConst root, void *user);

class FramedMsgPackReceiver
{
public:
    // Stack-buffer size used to drain oversized frame payloads in
    // SKIP_PAYLOAD chunks. Sized to absorb a typical UART burst in one
    // readBytes() without inflating the per-call stack frame — UART0
    // RX FIFO is 128 B on ESP32-C3, half that is plenty per pump tick.
    static constexpr size_t kSkipScratchSize = 64;

    FramedMsgPackReceiver(Stream &in, uint8_t *buffer, size_t bufferSize)
        : _in(in), _buf(buffer), _bufSize(bufferSize)
    {
    }

    void setHandler(OnMsgPackMessage cb, void *user = nullptr)
    {
        _cb = cb;
        _user = user;
    }

    // Non-blocking: processes up to maxFrames complete frames (default 1)
    // Returns number of complete frames dispatched.
    uint8_t poll(uint8_t maxFrames = 1)
    {
        uint8_t dispatched = 0;
        while (dispatched < maxFrames)
        {
            if (!processStep())
                break; // need more data or nothing left
            if (_frameComplete)
            {
                _frameComplete = false;
                dispatched++;
            }
        }
        return dispatched;
    }

    // Optional helper: drain everything currently buffered (use sparingly)
    uint8_t pollAll()
    {
        uint8_t total = 0;
        while (processStep())
        {
            if (_frameComplete)
            {
                _frameComplete = false;
                total++;
            }
            // Stop if no more bytes immediately available
            if (_in.available() == 0 && _state != READ_PAYLOAD && _state != SKIP_PAYLOAD)
                break;
        }
        return total;
    }

private:
    enum State
    {
        READ_LEN0,
        READ_LEN1,
        READ_PAYLOAD,
        SKIP_PAYLOAD
    };
    void reset()
    {
        _state = READ_LEN0;
        _len = 0;
        _pos = 0;
        _skip = 0;
    }
    bool processStep()
    {
        switch (_state)
        {
        case READ_LEN0:
            if (_in.available() < 1)
                return false;
            _len = static_cast<uint16_t>(_in.read() & 0xFF) << 8;
            _state = READ_LEN1;
            return true;

        case READ_LEN1:
            if (_in.available() < 1)
                return false;
            _len |= static_cast<uint16_t>(_in.read() & 0xFF);
            if (_len == 0)
            {
                reset();
                return true; // processed empty frame (ignored)
            }
            if (_len > _bufSize)
            {
                _skip = _len;
                _state = SKIP_PAYLOAD;
                return true;
            }
            _pos = 0;
            _state = READ_PAYLOAD;
            return true;

        case READ_PAYLOAD:
        {
            int avail = _in.available();
            if (avail <= 0)
                return false;
            size_t need = _len - _pos;
            size_t toRead = (size_t)avail < need ? (size_t)avail : need;
            size_t n = _in.readBytes(reinterpret_cast<char *>(&_buf[_pos]), toRead);
            _pos += n;
            if (_pos < _len)
                return true; // partial payload read, continue later
            // Full frame. Clear the doc BEFORE deserializing so a previous
            // frame's leftover keys don't leak in if deserializeMsgPack
            // fails — ArduinoJson 7's contract is "clear before reuse".
            _doc.clear();
            DeserializationError err = deserializeMsgPack(_doc, _buf, _len);
            if (!err && _cb)
                _cb(_doc.as<JsonVariantConst>(), _user);
            reset();
            _frameComplete = true;
            return true;
        }

        case SKIP_PAYLOAD:
        {
            int avail = _in.available();
            if (avail <= 0)
                return false;
            // Drain into a scratch buffer using readBytes() so we get back
            // the actual bytes consumed — Stream::read() returns -1 if the
            // FIFO drained between available() and the read, and the old
            // byte-by-byte loop would silently skip non-existent bytes,
            // de-syncing the next length parse.
            uint8_t scratch[kSkipScratchSize];
            size_t want = (size_t)avail < _skip ? (size_t)avail : _skip;
            if (want > sizeof(scratch))
                want = sizeof(scratch);
            size_t got = _in.readBytes(reinterpret_cast<char *>(scratch), want);
            if (got == 0)
                return false; // FIFO drained; come back when more arrives
            _skip -= got;
            if (_skip == 0)
            {
                reset();
                return true;
            }
            return true; // still skipping, continue later
        }
        }
        return false;
    }

    Stream &_in;
    uint8_t *_buf = nullptr;
    size_t _bufSize = 0;

    State _state = READ_LEN0;
    uint16_t _len = 0;
    size_t _pos = 0;
    size_t _skip = 0;
    bool _frameComplete = false;

    JsonDocument _doc;
    OnMsgPackMessage _cb = nullptr;
    void *_user = nullptr;
};
