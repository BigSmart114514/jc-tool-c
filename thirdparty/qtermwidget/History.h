#ifndef TEHISTORY_H
#define TEHISTORY_H

#include <QBitRef>
#include <QHash>
#include <QVector>

#include "Character.h"

namespace Konsole
{

class HistoryType;

class HistoryScroll
{
public:
    HistoryScroll(HistoryType*);
    virtual ~HistoryScroll();

    virtual bool hasScroll() const;

    virtual int  getLines() const = 0;
    virtual int  getLineLen(int lineno) const = 0;
    virtual void getCells(int lineno, int colno, int count, Character res[]) const = 0;
    virtual bool isWrappedLine(int lineno) const = 0;

    Character getCell(int lineno, int colno) const { Character res; getCells(lineno,colno,1,&res); return res; }

    virtual void addCells(const Character a[], int count) = 0;
    virtual void addCellsVector(const QVector<Character>& cells)
    {
        addCells(cells.data(),cells.size());
    }
    virtual void addLine(bool previousWrapped=false) = 0;

    const HistoryType& getType() const { return *m_histType; }

protected:
    HistoryType* m_histType;
};

class HistoryScrollBuffer : public HistoryScroll
{
public:
    typedef QVector<Character> HistoryLine;

    HistoryScrollBuffer(unsigned int maxNbLines = 1000);
    ~HistoryScrollBuffer() override;

    int  getLines() const override;
    int  getLineLen(int lineno) const override;
    void getCells(int lineno, int colno, int count, Character res[]) const override;
    bool isWrappedLine(int lineno) const override;

    void addCells(const Character a[], int count) override;
    void addCellsVector(const QVector<Character>& cells) override;
    void addLine(bool previousWrapped=false) override;

    void setMaxNbLines(unsigned int nbLines);
    unsigned int maxNbLines() const { return _maxLineCount; }

private:
    int bufferIndex(int lineNumber) const;

    HistoryLine* _historyBuffer;
    QBitArray _wrappedLine;
    int _maxLineCount;
    int _usedLines;
    int _head;
};

class HistoryScrollNone : public HistoryScroll
{
public:
    HistoryScrollNone();
    ~HistoryScrollNone() override;

    bool hasScroll() const override;

    int  getLines() const override;
    int  getLineLen(int lineno) const override;
    void getCells(int lineno, int colno, int count, Character res[]) const override;
    bool isWrappedLine(int lineno) const override;

    void addCells(const Character a[], int count) override;
    void addLine(bool previousWrapped=false) override;
};

class HistoryType
{
public:
    HistoryType();
    virtual ~HistoryType();

    virtual bool isEnabled()           const = 0;
    bool isUnlimited() const { return maximumLineCount() == 0; }
    virtual int maximumLineCount()    const = 0;

    virtual HistoryScroll* scroll(HistoryScroll *) const = 0;
};

class HistoryTypeNone : public HistoryType
{
public:
    HistoryTypeNone();

    bool isEnabled() const override;
    int maximumLineCount() const override;

    HistoryScroll* scroll(HistoryScroll *) const override;
};

class HistoryTypeBuffer : public HistoryType
{
    friend class HistoryScrollBuffer;

public:
    HistoryTypeBuffer(unsigned int nbLines);

    bool isEnabled() const override;
    int maximumLineCount() const override;

    HistoryScroll* scroll(HistoryScroll *) const override;

protected:
    unsigned int m_nbLines;
};

}

#endif
