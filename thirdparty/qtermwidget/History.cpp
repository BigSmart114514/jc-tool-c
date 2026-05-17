#include "History.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cerrno>
#include <QtDebug>

using namespace Konsole;

HistoryScroll::HistoryScroll(HistoryType* t)
    : m_histType(t)
{
}

HistoryScroll::~HistoryScroll()
{
}

bool HistoryScroll::hasScroll() const
{
    return true;
}

// HistoryScrollBuffer

HistoryScrollBuffer::HistoryScrollBuffer(unsigned int maxNbLines)
    : HistoryScroll(new HistoryTypeBuffer(maxNbLines))
    , _historyBuffer(nullptr)
    , _maxLineCount(maxNbLines)
    , _usedLines(0)
    , _head(0)
{
    _historyBuffer = new HistoryLine[_maxLineCount];
}

HistoryScrollBuffer::~HistoryScrollBuffer()
{
    delete[] _historyBuffer;
    delete m_histType;
}

void HistoryScrollBuffer::addCells(const Character a[], int count)
{
    HistoryLine& line = _historyBuffer[_head];
    line.resize(count);
    for (int i = 0; i < count; i++)
        line[i] = a[i];
}

void HistoryScrollBuffer::addCellsVector(const QVector<Character>& cells)
{
    HistoryLine& line = _historyBuffer[_head];
    line = cells;
}

void HistoryScrollBuffer::addLine(bool)
{
    _head = (_head + 1) % _maxLineCount;
    if (_usedLines < _maxLineCount)
        _usedLines++;
}

int HistoryScrollBuffer::getLines() const
{
    return _usedLines;
}

int HistoryScrollBuffer::getLineLen(int lineno) const
{
    if (lineno >= _usedLines || lineno < 0)
        return 0;
    return _historyBuffer[bufferIndex(lineno)].size();
}

void HistoryScrollBuffer::getCells(int lineno, int colno, int count, Character res[]) const
{
    if (lineno >= _usedLines || lineno < 0)
        return;
    const HistoryLine& line = _historyBuffer[bufferIndex(lineno)];
    if (colno + count > line.size())
        count = line.size() - colno;
    if (count <= 0)
        return;
    for (int i = 0; i < count; i++)
        res[i] = line[colno + i];
}

bool HistoryScrollBuffer::isWrappedLine(int lineno) const
{
    if (lineno >= _usedLines || lineno < 0)
        return false;
    return _wrappedLine.testBit(bufferIndex(lineno));
}

int HistoryScrollBuffer::bufferIndex(int lineNumber) const
{
    if (_usedLines < _maxLineCount)
        return lineNumber;
    return (_head + lineNumber) % _maxLineCount;
}

void HistoryScrollBuffer::setMaxNbLines(unsigned int nbLines)
{
    if (_maxLineCount == nbLines)
        return;

    HistoryLine* oldBuffer = _historyBuffer;
    int oldLines = _usedLines;
    int oldMax = _maxLineCount;

    _maxLineCount = nbLines;
    _historyBuffer = new HistoryLine[_maxLineCount];
    _usedLines = 0;
    _head = 0;

    int start = oldLines > _maxLineCount ? oldLines - _maxLineCount : 0;
    for (int i = start; i < oldLines; i++) {
        int srcIdx;
        if (oldLines < oldMax)
            srcIdx = i;
        else
            srcIdx = (_head + i - start) % oldMax;
        _historyBuffer[_usedLines++] = oldBuffer[srcIdx];
    }

    delete[] oldBuffer;
}

// HistoryScrollNone

HistoryScrollNone::HistoryScrollNone()
    : HistoryScroll(new HistoryTypeNone())
{
}

HistoryScrollNone::~HistoryScrollNone()
{
    delete m_histType;
}

bool HistoryScrollNone::hasScroll() const
{
    return false;
}

int HistoryScrollNone::getLines() const { return 0; }
int HistoryScrollNone::getLineLen(int) const { return 0; }
void HistoryScrollNone::getCells(int, int, int, Character[]) const {}
bool HistoryScrollNone::isWrappedLine(int) const { return false; }
void HistoryScrollNone::addCells(const Character[], int) {}
void HistoryScrollNone::addLine(bool) {}

// HistoryType

HistoryType::HistoryType() {}
HistoryType::~HistoryType() {}

// HistoryTypeNone

HistoryTypeNone::HistoryTypeNone() {}
bool HistoryTypeNone::isEnabled() const { return false; }
int HistoryTypeNone::maximumLineCount() const { return 0; }

HistoryScroll* HistoryTypeNone::scroll(HistoryScroll* old) const
{
    delete old;
    return new HistoryScrollNone();
}

// HistoryTypeBuffer

HistoryTypeBuffer::HistoryTypeBuffer(unsigned int nbLines)
    : m_nbLines(nbLines)
{
}

bool HistoryTypeBuffer::isEnabled() const
{
    return true;
}

int HistoryTypeBuffer::maximumLineCount() const
{
    return m_nbLines;
}

HistoryScroll* HistoryTypeBuffer::scroll(HistoryScroll* old) const
{
    if (old)
        delete old;
    return new HistoryScrollBuffer(m_nbLines);
}
