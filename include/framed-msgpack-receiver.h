#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

// Callback signature; 'root' valid only during the call
using OnMsgPackMessage = void (*)(JsonVariantConst root, void *user);

class FramedMsgPackReceiver
{
public:
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
            // Full frame
            {
                DeserializationError err = deserializeMsgPack(_doc, _buf, _len);
                if (!err && _cb)
                    _cb(_doc.as<JsonVariantConst>(), _user);
                _doc.clear();
            }
            reset();
            _frameComplete = true;
            return true;
        }

        case SKIP_PAYLOAD:
        {
            int avail = _in.available();
            if (avail <= 0)
                return false;
            size_t chunk = (size_t)avail < _skip ? (size_t)avail : _skip;
            while (chunk--)
                (void)_in.read();
            _skip -= (size_t)avail < _skip ? (size_t)avail : _skip; // adjust correctly
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
