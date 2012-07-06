//******************************************************************************
// Copyright (c) 2012 Paul Scherrer Institut PSI), Villigen, Switzerland
// Disclaimer: neither  PSI, nor any of their employees makes any warranty
// or assumes any legal liability or responsibility for the use of this software
//******************************************************************************
//******************************************************************************
//
//     Author : Anton Chr. Mezger
//
//******************************************************************************

#include "caqtdm_lib.h"
#include "dmsearchfile.h"
#include <math.h>
#include <stdlib.h>
#include <iostream>
#include <sstream>
#include <sys/timeb.h>
#include <postfix.h>
#include <QObject>
#include "alarmdefs.h"

// not nice, we should not need this partial declaration here
typedef struct _connectInfo {
    int cs;          // 0= epics, 1 =acs
    int connected;
} connectInfo;

#define PRINT(x)
#define min(x,y)   (((x) < (y)) ? (x) : (y))

#define ToolTipPrefix "<p style='background-color:yellow'><font color='#000000'>"
#define ToolTipPostfix "</font></font></p>"

#define InfoPrefix "<p style='background-color:lightyellow'><font color='#000000'>"
#define InfoPostfix "</font></font></p>"

#define ComputeVisibility(x)    \
    switch(widget->getVisibility()) { \
    case x::StaticV:\
    return true;\
    break; \
    case x::IfNotZero: \
    visible = (value != 0.0?true:false); \
    break;\
    case x::IfZero:\
    visible = (value == 0.0?true:false);\
    break;\
    case x::Calc:\
    visible = CalcVisibility(widget);\
    break;\
    }

#define BuildVector(x) \
    switch(data.edata.fieldtype) { \
    case caFLOAT: { \
    float* P = (float*) data.edata.dataB; \
    for(int i=0; i< data.edata.valueCount; i++) y.append(P[i]); \
    } \
    break; \
    case caDOUBLE: { \
    double* P = (double*) data.edata.dataB; \
    for(int i=0; i< data.edata.valueCount; i++) y.append(P[i]); \
    } \
    break; \
    case caLONG: { \
    long* P = (long*) data.edata.dataB; \
    for(int i=0; i< data.edata.valueCount; i++) y.append(P[i]); \
    } \
    break; \
    case caINT: { \
    int* P = (int*) data.edata.dataB; \
    for(int i=0; i< data.edata.valueCount; i++) y.append(P[i]); \
    } \
    break; \
    }

//===============================================================================================

Q_DECLARE_METATYPE(QList<int>)

extern "C" int CreateAndConnect(int index, knobData *data);
extern "C" void ClearMonitor(knobData *kData);
extern "C" void PrepareDeviceIO();
extern "C" int EpicsSetValue(char *pv, float rdata, long idata, char *sdata, char *object, char *errmess);
extern "C" void TerminateDeviceIO();

//extern MutexKnobData *mutexKnobData;
MutexKnobData *mutexKnobData;
MessageWindow *messageWindow;

class Sleep
{
public:
    static void msleep(unsigned long msecs)
    {
        QMutex mutex;
        mutex.lock();
        QWaitCondition waitCondition;
        waitCondition.wait(&mutex, msecs);
        mutex.unlock();
    }
};

/*
 * CaQtDM_Lib destructor
 */
CaQtDM_Lib::~CaQtDM_Lib()
{
    //qDebug() << "nb elements:" << includeWidgetList.count();
    for (int i = includeWidgetList.count()-1; i >= 0; --i)
    {
        QWidget *widget;
        widget= includeWidgetList.at(i);
        //qDebug() << "delete" << widget;
        delete widget;
    }
    delete myWidget;
    includeWidgetList.clear();
}

/*
 * CaQtDM_Lib constructor
 */
CaQtDM_Lib::CaQtDM_Lib(QWidget *parent, QString filename, QString macro, MutexKnobData *mKnobData, MessageWindow *msgWindow) : QMainWindow(parent)
{

    mutexKnobData = mKnobData;
    messageWindow = msgWindow;
    //qDebug() << "open file" << filename << "with macro" << macro;
    setAttribute(Qt::WA_DeleteOnClose);

    QUiLoader loader;

    // define a layout
    QGridLayout *layout = new QGridLayout;

    // define the file to use
    QFile *file = new QFile;
    file->setFileName(filename);

    file->open(QFile::ReadOnly);

    myWidget = loader.load(file, this);
    if (!myWidget) {
        QMessageBox::warning(this, tr("caQtDM"), tr("Error loading %1. Use designer to find errors").arg(filename));
        file->close();
        delete file;
        this->deleteLater();
        return;
    }
    file->close();

    // set window title without the whole path
    QString title(file->fileName().section('/',-1));
    title.append("&");
    title.append(macro);
    setWindowTitle(title);
    setUnifiedTitleAndToolBarOnMac(true);

    delete file;

    // define size of application
    setMinimumSize(myWidget->width(), myWidget->height());
    setMaximumSize(myWidget->width(), myWidget->height());

    // add widget to the gui
    layout->addWidget(myWidget);

    QWidget *centralWidget = new QWidget;
    centralWidget->setLayout(layout);
    centralWidget->layout()->setContentsMargins(0,0,0,0);
    setCentralWidget(centralWidget);

    qRegisterMetaType<knobData>("knobData");

    // connect signals to slots for exchanging data
    connect(mutexKnobData, SIGNAL(Signal_QLineEdit(const QString&, const QString&)), this,
            SLOT(Callback_UpdateLine(const QString&, const QString&)));

    connect(mutexKnobData,
            SIGNAL(Signal_UpdateWidget(int, QWidget*, const QString&, const QString&, const QString&, knobData)), this,
            SLOT(Callback_UpdateWidget(int, QWidget*, const QString&, const QString&, const QString&, knobData)));

    connect(this, SIGNAL(Signal_OpenNewWFile(const QString&, const QString&)), parent,
            SLOT(Callback_OpenNewFile(const QString&, const QString&)));


    // initialize IO
    PrepareDeviceIO();

    level=0;

    // say for all widgets that they have to be treated, will be set to true when treated to avoid multiple use
    // by findChildren
    QList<QWidget *> all = this->findChildren<QWidget *>();
    foreach(QWidget* widget, all) {
        widget->setProperty("Taken", false);
    }

    // get from the display all the widgets having a monitor associated
    QList<QWidget *> widgets = this->findChildren<QWidget *>();
    foreach(QWidget *w1, widgets) {
        savedMacro[0] = macro;
        HandleWidget(w1, savedMacro[0]);
    }
}

/*
 * this routine reaffects a text when macro is used
 */
bool CaQtDM_Lib::reaffectText(QMap<QString, QString> map, QString *text) {
    bool doNothing;
    if(text->size() > 0) {
        if(text->contains("$(") && text->contains(")")) {
            *text =  treatMacro(map, *text, &doNothing);
            return true;
        }
    }
    return false;
}


/*
 * this routine creates a QMap from a macro string
 */
QMap<QString, QString> CaQtDM_Lib::createMap(const QString& macro)
{
    QMap<QString, QString> map;
    // macro of type A=MMAC3,B=STR,C=RMJ:POSA:2 to be used for replacements of pv in child widgets
    if(macro != NULL) {
        QStringList vars = macro.split(",", QString::SkipEmptyParts);
        for(int i=0; i< vars.count(); i++) {
            QStringList keyvalue = vars.at(i).split("=", QString::SkipEmptyParts);
            if(keyvalue.count() == 2) {
                map.insert(keyvalue.at(0).trimmed(), keyvalue.at(1));
            }
        }
    }
    //qDebug() << "create map from macro:" << macro;
    //qDebug() << "resulting map=" << map;
    return map;
}

/*
 * this routine handles the initialization of all widgets
 */
void CaQtDM_Lib::HandleWidget(QWidget *w1, QString macro)
{
    QMap<QString, QString> map;
    knobData kData;
    int specData[5];
    memset(&kData, 0, sizeof (knobData));
    bool doNothing;
    QString pv;

    QVariant test=w1->property("Taken");
    if(!test.isNull()) {
        if(test.toBool()) return;
    }

    //qDebug() << w1->metaObject()->className() << w1->objectName();
    QString className(w1->metaObject()->className());

    if(className.contains("ca")) {
        PRINT(printf("\n%*c %s macro=<%s>", 15 * level, '+', qPrintable(w1->objectName()), qPrintable(macro)));
        map = createMap(macro);
    }

    QColor bg = w1->property("background").value<QColor>();
    QColor fg = w1->property("foreground").value<QColor>();

    // keep original colors
    w1->setProperty("BColor", bg);
    w1->setProperty("FColor", fg);

    // make a context menu
    if(className.contains("ca")) {
        w1->setContextMenuPolicy(Qt::CustomContextMenu);
        connect(w1, SIGNAL(customContextMenuRequested(const QPoint&)), this, SLOT(ShowContextMenu(const QPoint&)));
        w1->setProperty("Connect", false);
    }

    // the different widgets to be handled
    //==================================================================================================================
    if(caImage* widget = qobject_cast<caImage *>(w1)) {

        //qDebug() << "create caImage";

        InitVisibility(w1, &kData, map, specData);

        // empty calc string, set animation
        if(widget->getImageCalc().size() == 0) {
            //qDebug() <<  "no calc for image";
            widget->setFrame(0);
            widget->startMovie();
        }

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caRelatedDisplay* widget = qobject_cast<caRelatedDisplay *>(w1)) {

        //qDebug() << "create caRelatedDisplay" << widget;

        QString text;

        text = widget->getLabels();
        if(reaffectText(map, &text))  widget->setLabels(text);

        text = widget->getArgs();
        if(reaffectText(map, &text))  widget->setArgs(text);

        text = widget->getFiles();
        if(reaffectText(map, &text)) widget->setFiles(text);

        text = widget->getLabel();
        if(reaffectText(map, &text))  widget->setLabel(text);

        connect(widget, SIGNAL(clicked(int)), this, SLOT(Callback_RelatedDisplayClicked(int)));
        connect(widget, SIGNAL(triggered(int)), this, SLOT(Callback_RelatedDisplayClicked(int)));

        widget->raise();

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caShellCommand* widget = qobject_cast<caShellCommand *>(w1)) {

        //qDebug() << "create caShellCommand";

        QString text;
        text= widget->getLabels();
        if(reaffectText(map, &text))  widget->setLabels(text);

        text = widget->getArgs();
        if(reaffectText(map, &text))  widget->setArgs(text);

        text = widget->getFiles();
        if(reaffectText(map, &text))  widget->setFiles(text);

        text = widget->getLabel();
        if(reaffectText(map, &text))  widget->setLabel(text);

        connect(widget, SIGNAL(clicked(int)), this, SLOT(Callback_ShellCommandClicked(int)));
        connect(widget, SIGNAL(triggered(int)), this, SLOT(Callback_ShellCommandClicked(int)));

        widget->raise();

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caMenu* widget = qobject_cast<caMenu *>(w1)) {

        //qDebug() << "create caMenu";

        QString text = widget->getPV();
        if(text.size() > 0) {
            text =  treatMacro(map, text, &doNothing);
            addMonitor(myWidget, &kData, text, w1, specData, map, &pv);
            connect(widget, SIGNAL(activated(QString)), this, SLOT(Callback_MenuClicked(QString)));
            widget->setPV(text);
        }

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caCamera* widget = qobject_cast<caCamera *>(w1)) {

        //qDebug() << "create caCamera";

        // addmonitor normally will add a tooltip to show the pv; however here we have more than one pv
        QString tooltip;
        tooltip.append(ToolTipPrefix);

        for(int i=0; i< 5; i++) {
            QString text;
            if(i==0) text = widget->getPV_Data();
            if(i==1) text = widget->getPV_Width();
            if(i==2) text = widget->getPV_Height();
            if(i==3) text = widget->getPV_Code();
            if(i==4) text = widget->getPV_BPP();
            if(text.size() > 0) {
                specData[0] = i;   // type
                text =  treatMacro(map, text, &doNothing);
                addMonitor(myWidget, &kData, text, w1, specData, map, &pv);
                if(i>0) tooltip.append("<br>");
                tooltip.append(pv);
            } else if (i==3) {  // code missing (assume 1 for Helge)
                widget->setCode(1);
            } else if (i==4) {  // bpp missing (assume 3 for Helge)
                widget->setBPP(3);
            }
        }
        // finish tooltip
        tooltip.append(ToolTipPostfix);
        widget->setToolTip(tooltip);

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caChoice* widget = qobject_cast<caChoice *>(w1)) {

        //qDebug() << "create caChoice";

        QString text = widget->getPV();
        if(text.size() > 0) {
            text =  treatMacro(map, text, &doNothing);
            addMonitor(myWidget, &kData, text, w1, specData, map, &pv);
            connect(widget, SIGNAL(clicked(QString)), this, SLOT(Callback_ChoiceClicked(QString)));
            widget->setPV(text);
        }

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caLabel* widget = qobject_cast<caLabel *>(w1)) {

        //qDebug() << "create caLabel";

        InitVisibility(w1, &kData, map, specData);

        QString text =  treatMacro(map, widget->text(), &doNothing);
        text.replace(QString::fromWCharArray(L"\u00A6"), " ");    // replace ¦ with a blanc (was used in macros for creating blancs)
        widget->setText(text);

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caTextEntry* widget = qobject_cast<caTextEntry *>(w1)) {

        //qDebug() << "create caTextEntry";

        if(widget->getPV().size() > 0) {
            widget->setEnabled(true);
            addMonitor(myWidget, &kData, widget->getPV(), w1, specData, map, &pv);
            connect(widget, SIGNAL(TextEntryChanged(const QString&)), this,
                    SLOT(Callback_TextEntryChanged(const QString&)));
        }
        // default format, format from ui file will be used normally except for channel precision
        widget->setFormat(1);
        widget->setFocusPolicy(Qt::ClickFocus);
        widget->clearFocus();

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caLineEdit* widget = qobject_cast<caLineEdit *>(w1)) {

        //qDebug() << "create caLineEdit";

        if(widget->getPV().size() > 0) {
            //widget->setEnabled(false);
            widget->setFocusPolicy(Qt::NoFocus);
            widget->setCursor(QCursor());
            widget->setReadOnly(true);
            widget->setDragEnabled(true);

            widget->setAlignment(widget->alignment());
            addMonitor(myWidget, &kData, widget->getPV(), w1, specData, map, &pv);
            widget->setPV(pv);
        }

        // default format, format from ui file will be used normally except for channel precision
        widget->setFormat(1);

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caGraphics* widget = qobject_cast<caGraphics *>(w1)) {

        //qDebug() << "create caGraphics";

        InitVisibility(w1, &kData, map, specData);

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caPolyLine* widget = qobject_cast<caPolyLine *>(w1)) {

        //qDebug() << "create caPolyLine";

        InitVisibility(w1, &kData, map, specData);

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if (caApplyNumeric* widget = qobject_cast<caApplyNumeric *>(w1)){

        //qDebug() << "create caAppyNumeric";

        if(widget->getPV().size() > 0) {
            addMonitor(myWidget, &kData, widget->getPV(), w1, specData, map, &pv);
            widget->setPV(pv);
            connect(widget, SIGNAL(clicked(double)), this, SLOT(Callback_EApplyNumeric(double)));
        }
        widget->raise();

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if (caNumeric* widget = qobject_cast<caNumeric *>(w1)){

        if(widget->getPV().size() > 0) {
            addMonitor(myWidget, &kData, widget->getPV(), w1, specData, map, &pv);
            widget->setPV(pv);
            connect(widget, SIGNAL(valueChanged(double)), this, SLOT(Callback_ENumeric(double)));
        }
        widget->raise();

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if (caMessageButton* widget = qobject_cast<caMessageButton *>(w1)) {

        QString text;
        //qDebug() << "create caMessageButton";
        if(widget->getPV().size() > 0) {
            addMonitor(myWidget, &kData, widget->getPV(), w1, specData, map, &pv);
            widget->setPV(pv);
        }
        connect(widget, SIGNAL(messageButtonSignal(int)), this, SLOT(Callback_MessageButton(int)));

        text = widget->getLabel();
        if(reaffectText(map, &text))  widget->setLabel(text);

        widget->raise();

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caLed* widget = qobject_cast<caLed *>(w1)) {

        //qDebug() << "create caLed";

        if(widget->getPV().size() > 0) {
            addMonitor(myWidget, &kData, widget->getPV(), w1, specData, map, &pv);
            widget->setPV(pv);
        }

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caBitnames* widget = qobject_cast<caBitnames *>(w1)) {

        //qDebug() << "create caBitnames";

        if(widget->getEnumPV().size() > 0 && widget->getValuePV().size()) {
            addMonitor(myWidget, &kData, widget->getEnumPV(), w1, specData, map, &pv);
            widget->setEnumPV(pv);
            addMonitor(myWidget, &kData, widget->getValuePV(), w1, specData, map, &pv);
            widget->setValuePV(pv);
        }

        widget->setProperty("Taken", true);
        //==================================================================================================================
    } else if(caSlider* widget = qobject_cast<caSlider *>(w1)) {

        //qDebug() << "create caSlider";

        if(widget->getPV().size() > 0) {
            addMonitor(myWidget, &kData, widget->getPV(), w1, specData, map, &pv);
            widget->setPV(pv);
            connect(widget, SIGNAL(sliderMoved(double)), this, SLOT(Callback_Slider(double)));
        }

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caThermo* widget = qobject_cast<caThermo *>(w1)) {

        //qDebug() << "create caThermo";

        if(widget->getPV().size() > 0) {
            addMonitor(myWidget, &kData, widget->getPV(), w1, specData, map, &pv);
            widget->setPV(pv);
        }
        // for an opposite direction, invert maximum and minimum

        if(widget->getDirection() == caThermo::Down || widget->getDirection() == caThermo::Left) {
            double max = widget->maxValue();
            double min = widget->minValue();
            widget->setMinValue(max);
            widget->setMaxValue(min);
        }

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caLinearGauge* widget = qobject_cast<caLinearGauge *>(w1)) {

        //qDebug() << "create lineargauge for" << widget->getPV();

        if(widget->getPV().size() > 0) {
            addMonitor(myWidget, &kData, widget->getPV(), w1, specData, map, &pv);
            widget->setPV(pv);
        }

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caCircularGauge* widget = qobject_cast<caCircularGauge *>(w1)) {

        //qDebug() << "create circulargauge for" << widget->getPV();

        if(widget->getPV().size() > 0) {
            addMonitor(myWidget, &kData, widget->getPV(), w1, specData, map, &pv);
            widget->setPV(pv);
        }

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caByte* widget = qobject_cast<caByte *>(w1)) {

        //qDebug() << "create caByte" << w1;

        if(widget->getPV().size() > 0) {
            addMonitor(myWidget, &kData, widget->getPV(), w1, specData, map, &pv);
            widget->setPV(pv);
        }

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caInclude* widget = qobject_cast<caInclude *>(w1)) {
        QWidget *thisW;
        QFile *file = new QFile;
        QUiLoader loader;

        // define a layout
        QGridLayout *layout = new QGridLayout;
        layout->setContentsMargins(0,0,0,0);

        //qDebug() << "treat caInclude" << w1 << "level=" << level;

        InitVisibility(w1, &kData, map, specData);

        widget->setProperty("Taken", true);

        QString macroS = widget->getMacro();
        if(macroS.size() < 1) {
            if(level>0){
                PRINT(printf("\n    %*c get last macro=%s", 15 * level, ' ', qPrintable(savedMacro[level-1])));
                macroS = savedMacro[level-1];
            } else {
                macroS = savedMacro[level];
            }
        } else {
            //printf("\n    %*c macro=%s", 15 * level, ' ',  qPrintable(macroS));
            savedMacro[level] = macroS;
        }

        macroS = treatMacro(map, macroS, &doNothing);
        savedMacro[level] = treatMacro(map, savedMacro[level], &doNothing);

        // define the file to use
        QString fileName = widget->getFileName().trimmed();
        QStringList openFile = fileName.split(".", QString::SkipEmptyParts);
        fileName = openFile[0].append(".ui");

        searchFile *s = new searchFile(fileName);

        if(s->findFile().isNull()) {
            qDebug() << "file" << fileName << "does not exist";
        } else {
            //qDebug() << "load file" << fileName << "with macro" << macroS;
            fileName = s->findFile();
        }
        delete s;

        // sure file exists ?
        QFileInfo fi(fileName);
        if(fi.exists()) {

            // open and load file
            file->setFileName(fileName);
            file->open(QFile::ReadOnly);
            thisW = loader.load(file, this);
            // some error with loading
            if (!thisW) {
                file->close();
                postMessage(QtDebugMsg, (char*) tr("could not load include file %1").arg(fileName).toAscii().constData());
                // seems to be ok
            } else {
                includeWidgetList.append(thisW);
                file->close();

                // add widget to the gui
                layout->addWidget(thisW);
                widget->setLayout(layout);
                widget->setLineSize(0);

                // go through its childs
                QList<QWidget *> childs = thisW->findChildren<QWidget *>();
                level++;

                foreach(QWidget *child, childs) {
                    HandleWidget(child, macroS);
                }
                level--;
            }

        } else {
            qDebug() << "sorry, file" << fileName << " does not exist";
        }

        delete file;

        macroS = savedMacro[level];

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caFrame* widget = qobject_cast<caFrame *>(w1)) {

        //qDebug() << "treat caFrame" << w1;

        InitVisibility(w1, &kData, map, specData);

        widget->setProperty("Taken", true);

        QString macroS = widget->getMacro();

        if(macroS.size() < 1) {
            if(level > 0){
                //printf("\n    %*c get macro from previous level=%s w=<%s> ", 15 * level, ' ', qPrintable(savedMacro[level-1]), qPrintable(w1->objectName()));
                macroS = savedMacro[level-1];
                savedMacro[level] = macroS;
            } else {
                macroS = savedMacro[level];
            }
        } else {
            //printf("\n    %*c macro=%s <%s>", 15 * level, ' ', qPrintable(macroS), qPrintable(w1->objectName()));
            savedMacro[level] = macroS;
        }

        macroS = treatMacro(map, macroS, &doNothing);
        savedMacro[level] = treatMacro(map, savedMacro[level], &doNothing);

        QList<QWidget *> childs = widget->findChildren<QWidget *>();
        level++;

        foreach(QWidget *child, childs) {
            HandleWidget(child, macroS);
        }
        level--;

        macroS= savedMacro[level];

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caCartesianPlot* widget = qobject_cast<caCartesianPlot *>(w1)) {

        //qDebug() << "create cartesian plot";

        // addmonitor normally will add a tooltip to show the pv; however here we have more than one pv
        QString tooltip;
        tooltip.append(ToolTipPrefix);

        // i do this while the order has to be correct
        widget->setForeground(widget->getForeground());
        widget->setBackground(widget->getBackground());
        widget->setScaleColor(widget->getScaleColor());
        widget->setGridColor(widget->getGridColor());

        widget->setColor_1(widget->getColor_1());
        widget->setStyle_1(widget->getStyle_1());
        widget->setSymbol_1(widget->getSymbol_1());

        widget->setColor_2(widget->getColor_2());
        widget->setStyle_2(widget->getStyle_2());
        widget->setSymbol_2(widget->getSymbol_2());

        widget->setColor_3(widget->getColor_3());
        widget->setStyle_3(widget->getStyle_3());
        widget->setSymbol_3(widget->getSymbol_3());

        widget->setColor_4(widget->getColor_4());
        widget->setStyle_4(widget->getStyle_4());
        widget->setSymbol_4(widget->getSymbol_4());

        widget->setWhiteColors();

        // go through 4 possible curves and add monitor
        for(int i=0; i< 4; i++) {
            QStringList thisString;
            QString pvs ="";
            if(i==0) thisString = widget->getPV_1().split(";");
            if(i==1) thisString = widget->getPV_2().split(";");
            if(i==2) thisString = widget->getPV_3().split(";");
            if(i==3) thisString = widget->getPV_4().split(";");
            if(thisString.count() == 2 && thisString.at(0).trimmed().length() > 0 && thisString.at(1).trimmed().length() > 0) {
                specData[0] = i; // curve number
                specData[1] = caCartesianPlot::XY_both;
                specData[2] = 0; // X
                addMonitor(myWidget, &kData, thisString.at(0), w1, specData, map, &pv);
                tooltip.append(pv);
                pvs = pv;
                specData[2] = 1; // Y
                addMonitor(myWidget, &kData, thisString.at(1), w1, specData, map, &pv);
                tooltip.append(",");
                tooltip.append(pv);
                pvs.append(";");
                pvs.append(pv);
                tooltip.append("<br>");
            } else if(thisString.count() == 2 && thisString.at(0).trimmed().length() > 0 && thisString.at(1).trimmed().length() == 0) {
                specData[0] = i; // curve number
                specData[1] = caCartesianPlot::X_only;
                specData[2] = 0; // X
                addMonitor(myWidget, &kData, thisString.at(0), w1, specData, map, &pv);
                pvs.append(pv);
                pvs.append(";");
                tooltip.append(pv);
                tooltip.append("<br>");
            } else if(thisString.count() == 2 && thisString.at(1).trimmed().length() > 0 && thisString.at(0).trimmed().length() == 0) {
                specData[0] = i; // curve number
                specData[1] = caCartesianPlot::Y_only;
                specData[2] = 1; // Y
                addMonitor(myWidget, &kData, thisString.at(1), w1, specData, map, &pv);
                pvs.append(";");
                pvs.append(pv);
                tooltip.append(pv);
                tooltip.append("<br>");
            } else {
                //curvtype = XY_nothing;
            }
            if(i==0) widget->setPV_1(pvs);
            if(i==1) widget->setPV_2(pvs);
            if(i==2) widget->setPV_3(pvs);
            if(i==3) widget->setPV_4(pvs);
        }

        // finish tooltip
        tooltip.append(ToolTipPostfix);
        widget->setToolTip(tooltip);

        // handle user scale
        if(widget->getXscaling() == caCartesianPlot::User) {
            double xmin, xmax;
            int ok=widget->getXLimits(xmin, xmax);
            widget->setScaleX(xmin, xmax);
        } else if(widget->getXscaling() == caCartesianPlot::Auto) {
            widget->setXscaling(caCartesianPlot::Auto);
        }
        if(widget->getYscaling() == caCartesianPlot::User) {
            double ymin, ymax;
            int ok=widget->getYLimits(ymin, ymax);
            widget->setScaleY(ymin, ymax);
        } else if(widget->getYscaling() == caCartesianPlot::Auto){
            widget->setYscaling(caCartesianPlot::Auto);
        }

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caStripPlot* widget = qobject_cast<caStripPlot *>(w1)) {

        //qDebug() << "create caStripPlot";

        QString text;

        // addmonitor normally will add a tooltip to show the pv; however here we have more than one pv
        QString tooltip;
        tooltip.append(ToolTipPrefix);

        text = widget->getPVS();
        reaffectText(map, &text);
        QStringList vars = text.split(";", QString::SkipEmptyParts);

        int NumberOfCurves = min(vars.count(), 5);

        widget->defineCurves(vars, widget->getUnits(), widget->getPeriod(),  widget->width(),  NumberOfCurves);

        // go through the defined curves and add monitor

        for(int i=0; i< NumberOfCurves; i++) {
            QString pv = vars.at(i).trimmed();
            if(pv.size() > 0) {
                if(i==0) {
                    //qDebug() <<  widget->getYaxisLimitsMin(i) << widget->getYaxisLimitsMax(i);
                    widget->setYscale( widget->getYaxisLimitsMin(i), widget->getYaxisLimitsMax(i));
                }
                specData[1] = i;            // curve number
                specData[0] = vars.count(); // number of curves
                addMonitor(myWidget, &kData, pv, w1, specData, map, &pv);
                widget->showCurve(i, true);

                tooltip.append(pv);
                tooltip.append("<br>");
            }
        }

        // finish tooltip
        tooltip.append(ToolTipPostfix);
        widget->setToolTip(tooltip);

        widget->setProperty("Taken", true);

        //==================================================================================================================
    } else if(caTable* widget = qobject_cast<caTable *>(w1)) {

        //qDebug() << "create caTable" << widget->getPVS();

        QStringList vars = widget->getPVS().split(";", QString::SkipEmptyParts);
        widget->setColumnCount(3);
        widget->setRowCount(vars.count());

        // go through the pvs and add monitor
        for(int i=0; i< vars.count(); i++) {
            QString pv = vars.at(i);
            if(pv.size() > 0) {
                QTableWidgetItem *item;
                specData[0] = i;            // table row
                addMonitor(myWidget, &kData, pv, w1, specData, map, &pv);
                item = new QTableWidgetItem(pv);
                item->setTextAlignment(Qt::AlignAbsolute | Qt:: AlignLeft);
                widget->setItem(i,0, item);
            }
        }
        widget->setColumnSizes(widget->getColumnSizes());
        widget->setProperty("Taken", true);
    }
}

QString CaQtDM_Lib::treatMacro(QMap<QString, QString> map, const QString& text, bool *doNothing)
{
    QString newText = text;
    *doNothing = false;

    // a macro exists and when pv contains a right syntax then replace pv
    if(!map.isEmpty()) {
        if(text.contains("$(") && text.contains(")")) {
            QMapIterator<QString, QString> i(map);
            while (i.hasNext()) {
                i.next();
                QString toReplace = "$(" + i.key() + ")";
                //qDebug() << "replace in" << newText << toReplace << "with" << i.value();
                newText.replace(toReplace, i.value());
            }
        }
    } else {
        if(text.contains("$")) *doNothing = true;
    }
    return newText;
}



/*
 * routine to create an epics monitor
 */
int CaQtDM_Lib::addMonitor(QWidget *thisW, knobData *kData, QString pv, QWidget *w, int *specData, QMap<QString, QString> map, QString *pvRep)
{
    // define epics monitors
    bool doNothing;

    if(pv.size() == 0) return -1;

    QString trimmedPV = pv.trimmed();

    QString newpv = treatMacro(map, trimmedPV, &doNothing);
    if(doNothing) return -1;
    *pvRep = newpv;

    // set pvname as tooltip
    QString tooltip;
    tooltip.append(ToolTipPrefix);
    tooltip.append(newpv);
    tooltip.append(ToolTipPostfix);
    w->setToolTip(tooltip);

    strcpy(kData->pv, (char*) newpv.toAscii().constData());
    memcpy(kData->specData, specData, sizeof(int) * 5);
    kData->thisW = (void*) thisW;
    kData->dispW = (void*) w;
    strcpy(kData->dispName, w->objectName().toLower().toAscii().constData());

    if (QLineEdit *lineedit = qobject_cast<QLineEdit *>(w)) {
        lineedit->setText("");
    }

    int num = mutexKnobData->GetMutexKnobDataIndex();
    if(num == -1) {
        qDebug() << "this should never happen";
        return num;
    }
    mutexKnobData->SetMutexKnobData(num, *kData);

    CreateAndConnect(num, kData);

    w->setProperty("MonitorIndex", num);
    w->setProperty("Connect", false);

    return num;
}

bool CaQtDM_Lib::bitState(int value, int bitNr)
{
    return ((((int) value >> bitNr) & 1) == 1);
}

int CaQtDM_Lib::setObjectVisibility(QWidget *w, double value)
{
    bool visible = true;

    if(caFrame *widget = qobject_cast<caFrame *>(w)) {
        // treat visibility if defined
        ComputeVisibility(caFrame)
    } else if (caInclude *widget = qobject_cast<caInclude *>(w)) {
        // treat visibility if defined
        ComputeVisibility(caInclude)
    } else if (caGraphics *widget = qobject_cast<caGraphics *>(w)) {
        // treat visibility if defined
        ComputeVisibility(caGraphics)
    } else if(caImage *widget = qobject_cast<caImage *>(w)) {
        // treat visibility if defined
        ComputeVisibility(caImage)
    } else if(caPolyLine *widget = qobject_cast<caPolyLine *>(w)) {
        // treat visibility if defined
        ComputeVisibility(caPolyLine)
    } else if(caLabel *widget = qobject_cast<caLabel *>(w)) {
        // treat visibility if defined
        ComputeVisibility(caLabel)
    }

    if(!visible) {
        w->hide();
    } else {
        w->show();
    }
    return visible;
}

bool CaQtDM_Lib::CalcVisibility(QWidget *w)
{
    double valueArray[MAX_CALC_INPUTS];
    char post[256];
    char calcString[256];
    long status;
    short errnum;
    double result;
    bool visible;

    if(caFrame *frame = qobject_cast<caFrame *>(w)) {
        strcpy(calcString, frame->getVisibilityCalc().toAscii().constData());
    }
    if(caInclude *frame = qobject_cast<caInclude *>(w)) {
        strcpy(calcString, frame->getVisibilityCalc().toAscii().constData());
    }
    else if(caImage *image = qobject_cast<caImage *>(w)) {
        strcpy(calcString, image->getVisibilityCalc().toAscii().constData());
    }
    else if(caGraphics *image = qobject_cast<caGraphics *>(w)) {
        strcpy(calcString, image->getVisibilityCalc().toAscii().constData());
    }
    else if(caPolyLine *line = qobject_cast<caPolyLine *>(w)) {
        strcpy(calcString, line->getVisibilityCalc().toAscii().constData());
    }
    else if(caLabel *label = qobject_cast<caLabel *>(w)) {
        strcpy(calcString, label->getVisibilityCalc().toAscii().constData());
    }

    // any monitors ?
    QVariant var=w->property("MonitorList");
    QVariantList list = var.toList();
    int nbMonitors = list.at(0).toInt();
    //qDebug() << "number of monitors" << nbMonitors;
    if(nbMonitors > 0)  {

        // scan and get the channels
        for(int i=0; i < 4; i++) valueArray[i] = 0.0;
        for(int i=0; i< nbMonitors;i++) {
            knobData *ptr = mutexKnobData->GetMutexKnobDataPtr(list.at(i+1).toInt());
            //qDebug() << ptr->pv << ptr->edata.connected << ptr->edata.rvalue;
            // when connected
            if(ptr->edata.connected) {
                valueArray[i] = ptr->edata.rvalue;
            } else {
                valueArray[i] = 0.0;
            }
        }

        status = postfix(calcString, post, &errnum);
        if(status) {
            printf("invalid calc <%s> for <%s>\n", calcString, qPrintable(w->objectName()));
            return true;
        }
        // Perform the calculation
        status = calcPerform(valueArray, &result, post);
        if(!status) {
            visible = (result?true:false);
            //qDebug() << "valid result" << result << visible;
            return visible;
        } else {
            printf("invalid calc <%s> for <%s>\n", calcString, qPrintable(w->objectName()));
            return true;
        }
    }
    return true;
}

int CaQtDM_Lib::ComputeAlarm(QWidget *w)
{
    int status;
    // any monitors ?
    QVariant var=w->property("MonitorList");
    QVariantList list = var.toList();
    int nbMonitors = list.at(0).toInt();
    //qDebug() << "number of monitors" << nbMonitors;
    status = NO_ALARM;
    if(nbMonitors > 0)  {

        // medm uses however only first channel
        knobData *ptr = mutexKnobData->GetMutexKnobDataPtr(list.at(1).toInt());
        //qDebug() << ptr->pv << ptr->edata.connected << ptr->edata.rvalue;
        // when connected
        if(ptr->edata.connected) {
            status = status | ptr->edata.severity;
        } else {
            return NOTCONNECTED;
        }
    }
    return status;
}


/**
 * updates my widgets through monitor and emit signal
 */
void CaQtDM_Lib::Callback_UpdateWidget(int indx, QWidget *w,
                                       const QString& units,
                                       const QString& fec,
                                       const QString& String,
                                       const knobData& data)
{
    /*
    knobData *kPtr = mutexKnobData->GetMutexKnobDataPtr(indx);  // use pointer for getting all necessary information
*/
    // frame ==================================================================================================================
    if(caLabel *widget = qobject_cast<caLabel *>(w)) {
        //qDebug() << "we have a label";

        // treat visibility if defined
        setObjectVisibility(widget, data.edata.rvalue);

        // frame ==================================================================================================================
    } else if(caInclude *widget = qobject_cast<caInclude *>(w)) {
        //qDebug() << "we have an included frame";

        // treat visibility if defined
        setObjectVisibility(widget, data.edata.rvalue);

        // frame ==================================================================================================================
    } else if(caFrame *widget = qobject_cast<caFrame *>(w)) {
        //qDebug() << "we have a frame";

        // treat visibility if defined
        setObjectVisibility(widget, data.edata.rvalue);

        // menu ==================================================================================================================
    } else if (caMenu *widget = qobject_cast<caMenu *>(w)) {
        //qDebug() << "we have a menu" << String << value;

        QStringList stringlist = String.split( ";");
        // set enum strings
        if(data.edata.fieldtype == caENUM) {
            widget->populateCells(stringlist);
            if(widget->getLabelDisplay()) widget->setCurrentIndex(0);
            else widget->setCurrentIndex((int) data.edata.ivalue);
        }
        widget->setAccessW(data.edata.accessW);

        // choice ==================================================================================================================
    } else if (caChoice *widget = qobject_cast<caChoice *>(w)) {
        //qDebug() << "we have a choiceButton" << String << value;

        QStringList stringlist = String.split( ";");
        // set enum strings
        if(data.edata.fieldtype == caENUM) {
            widget->populateCells(stringlist, (int) data.edata.ivalue);
        }
        widget->setAccessW(data.edata.accessW);

        // thermometer ==================================================================================================================
    } else if (caThermo *widget = qobject_cast<caThermo *>(w)) {
        //qDebug() << "we have a thermometer";

        if(data.edata.connected) {
            bool channelLimitsEnabled = false;
            if(widget->getLimitsMode() == caThermo::Channel) channelLimitsEnabled= true;
            // take limits from channel, in case of user limits these should already been set
            if((channelLimitsEnabled) && (data.edata.initialize) ) {
                // when limits are the same, do nothing
                if(data.edata.upper_disp_limit != data.edata.lower_disp_limit) {
                    if(widget->getDirection() == caThermo::Down  || widget->getDirection() == caThermo::Left) {
                        widget->setMinValue(data.edata.upper_disp_limit);
                        widget->setMaxValue(data.edata.lower_disp_limit);
                    } else {
                        widget->setMaxValue(data.edata.upper_disp_limit);
                        widget->setMinValue(data.edata.lower_disp_limit);
                    }
                }
            }
            widget->setValue((double) data.edata.rvalue);

            // set colors when connected
            // case of alarm mode
            if (widget->getColorMode() == caThermo::Alarm) {
                if(channelLimitsEnabled) {
                    widget->setAlarmColors(data.edata.severity);
                } else {
                    widget->setUserAlarmColors((double) data.edata.rvalue);
                }

                // case of static mode
            } else {
                // done at initialisation, we have to set it back after no connect
                if(!widget->property("Connect").value<bool>()) {
                    QColor fg = widget->getForeground();
                    QColor bg = widget->getBackground();
                    widget->setColors(bg, fg);
                    widget->setProperty("Connect", true);
                }
            }
            // set no connection color
        } else {
            widget->setAlarmColors(NOTCONNECTED);
            widget->setProperty("Connect", false);
        }

        // Slider ==================================================================================================================
    } else if (caSlider *widget = qobject_cast<caSlider *>(w)) {

        bool channelLimitsEnabled = false;
        if(widget->getLimitsMode() == caSlider::Channel) channelLimitsEnabled= true;

        if(data.edata.connected) {
            bool channelLimitsEnabled = false;
            if(widget->getLimitsMode() == caSlider::Channel) channelLimitsEnabled= true;
            // take limits from channel, in case of user limits these should already been set
            if((channelLimitsEnabled) && (data.edata.initialize) ) {
                // when limits are the same, do nothing
                if(data.edata.upper_disp_limit != data.edata.lower_disp_limit) {
                    if(widget->getDirection() == caSlider::Down  || widget->getDirection() == caSlider::Left) {
                        widget->setMinValue(data.edata.upper_disp_limit);
                        widget->setMaxValue(data.edata.lower_disp_limit);
                    } else {
                        widget->setMaxValue(data.edata.upper_disp_limit);
                        widget->setMinValue(data.edata.lower_disp_limit);
                    }
                }
            }
            widget->setValue((double) data.edata.rvalue);
            widget->setAccessW(data.edata.accessW);

            // set colors when connected
            // case of alarm mode
            if (widget->getColorMode() == caSlider::Alarm) {
                if(channelLimitsEnabled) {
                    widget->setAlarmColors(data.edata.severity);
                } else {
                    widget->setUserAlarmColors((double) data.edata.rvalue);
                }

                // case of static, default mode
            } else {
                // done at initialisation, we have to set it back after no connect
                if(!widget->property("Connect").value<bool>()) {
                    QColor fg = widget->getForeground();
                    QColor bg = widget->getBackground();
                    widget->setColors(bg, fg);
                    widget->setProperty("Connect", true);
                }
            }
            // set no connection color
        } else {
            widget->setAlarmColors(NOTCONNECTED);
            widget->setProperty("Connect", false);
        }

        // linear gauge (like thermometer) ==================================================================================================================
    } else if (caLinearGauge *widget = qobject_cast<caLinearGauge *>(w)) {
        //qDebug() << "we have a linear gauge" << value;

        if(data.edata.connected) {
            bool externalLimitsEnabled = widget->property("externalLimitsEnabled").value<bool>();
            if((externalLimitsEnabled) && (data.edata.initialize)) {
                widget->setLowWarning(data.edata.lower_warning_limit);
                widget->setLowError(data.edata.lower_alarm_limit);
                widget->setHighError(data.edata.upper_alarm_limit);
                widget->setHighWarning(data.edata.upper_warning_limit);
                if(data.edata.upper_disp_limit != data.edata.lower_disp_limit) {
                    widget->setMaxValue(data.edata.upper_disp_limit);
                    widget->setMinValue(data.edata.lower_disp_limit);
                } else {
                    widget->setMaxValue(1000.0);
                    widget->setMinValue(0.0);
                }
            }
            widget->setValue((int) data.edata.rvalue);
        }

        // circular gauge  ==================================================================================================================
    } else if (caCircularGauge *widget = qobject_cast<caCircularGauge *>(w)) {
        //qDebug() << "we have a linear gauge" << value;

        if(data.edata.connected) {
            bool externalLimitsEnabled = widget->property("externalLimitsEnabled").value<bool>();
            if((externalLimitsEnabled) && (data.edata.initialize)) {
                widget->setLowWarning(data.edata.lower_warning_limit);
                widget->setLowError(data.edata.lower_alarm_limit);
                widget->setHighError(data.edata.upper_alarm_limit);
                widget->setHighWarning(data.edata.upper_warning_limit);
                if(data.edata.upper_disp_limit != data.edata.lower_disp_limit) {
                    widget->setMaxValue(data.edata.upper_disp_limit);
                    widget->setMinValue(data.edata.lower_disp_limit);
                } else {
                    widget->setMaxValue(1000.0);
                    widget->setMinValue(0.0);
                }
            }
            widget->setValue((int) data.edata.rvalue);
        }

        // byte ==================================================================================================================
    } else if (caByte *widget = qobject_cast<caByte *>(w)) {

        if(data.edata.connected) {
            int colorMode = widget->getColorMode();
            if(colorMode == caByte::Static) {
                if(!widget->property("Connect").value<bool>()) {
                    widget->setProperty("Connect", true);
                }
            } else if(colorMode == caByte::Alarm) {
                widget->setAlarmColors(data.edata.severity);
            }
            widget->setValue((int) data.edata.ivalue);
        } else {
            widget->setAlarmColors(NOTCONNECTED);
            widget->setProperty("Connect", false);
        }

        // lineEdit and textEntry ====================================================================================================
    } else if (caLineEdit *widget = qobject_cast<caLineEdit *>(w)) {
        //qDebug() << "we have a lineedit" << widget << kPtr->pv << data.edata.fieldtype << data.edata.dataSize << String;

        QColor bg = widget->property("BColor").value<QColor>();
        QColor fg = widget->property("FColor").value<QColor>();

        if(data.edata.connected) {
            // enum string
            if(data.edata.fieldtype == caENUM || data.edata.fieldtype == caSTRING || data.edata.fieldtype == caCHAR) {
                QStringList list;
                int colorMode = widget->getColorMode();
                if(colorMode == caLineEdit::Static || colorMode == caLineEdit::Default) { // done at initialisation
                    if(!widget->property("Connect").value<bool>()) {                    // but was disconnected before
                        widget->setAlarmColors(data.edata.severity, (double) data.edata.ivalue, bg, fg);
                        widget->setProperty("Connect", true);
                    }
                } else {
                    widget->setAlarmColors(data.edata.severity, (double) data.edata.ivalue, bg, fg);
                }
                list = String.split(";", QString::SkipEmptyParts);
                if((data.edata.fieldtype == caENUM)  && ((int) data.edata.ivalue < list.count())) {
                    widget->setText(list.at((int) data.edata.ivalue).trimmed());
                } else if (data.edata.fieldtype == caENUM) {
                    widget->setText("???");
                } else {
                    if(data.edata.valueCount == 1) {
                        widget->setText(String);
                    } else if(list.count() > 0) {
                        widget->setText(list.at(0).trimmed());
                    }
                }
                widget->setCursorPosition(0);

                // double
            } else {

                int colorMode = widget->getColorMode();
                int precMode = widget->getPrecisionMode();

                if(colorMode == caLineEdit::Static || colorMode == caLineEdit::Default) { // done at initialisation
                    if(!widget->property("Connect").value<bool>()) {                    // but was disconnected before
                        widget->setAlarmColors(data.edata.severity, data.edata.rvalue, bg, fg);
                        widget->setProperty("Connect", true);
                    }
                } else {
                    widget->setAlarmColors(data.edata.severity, data.edata.rvalue, bg, fg);
                }

                if((precMode != caLineEdit::User) && (data.edata.initialize)) {
                    widget->setFormat(data.edata.precision);
                }

                widget->setValue(data.edata.rvalue, units);
                // access control for textentry
                if (caTextEntry *widget = qobject_cast<caTextEntry *>(w)) {
                    widget->setAccessW(data.edata.accessW);
                }
            }

        } else {
            widget->setText("");
            widget->setAlarmColors(NOTCONNECTED, 0.0, bg, fg);
            widget->setProperty("Connect", false);
        }

        // Graphics ==================================================================================================================
    } else if (caGraphics *widget = qobject_cast<caGraphics *>(w)) {
        //qDebug() << "caGraphics" << widget->objectName() << widget->getColorMode();

        if(data.edata.connected) {
            int colorMode = widget->getColorMode();
            if(colorMode == caGraphics::Static) {
                // done at initialisation, we have to set it back after no connect
                if(!widget->property("Connect").value<bool>()) {
                    QColor fg = widget->property("FColor").value<QColor>();
                    QColor bg = widget->property("FColor").value<QColor>();
                    widget->setForeground(fg);
                    widget->setLineColor(fg);
                    widget->setProperty("Connect", true);
                }
            } else if(colorMode == caGraphics::Alarm) {
                int status = ComputeAlarm(w);
                widget->setAlarmColors(status);
            }

            // treat visibility if defined
            setObjectVisibility(widget, data.edata.rvalue);

        } else {
            widget->setAlarmColors(NOTCONNECTED);
            widget->setProperty("Connect", false);
        }

        // Polyline ==================================================================================================================
    } else if (caPolyLine *widget = qobject_cast<caPolyLine *>(w)) {

        if(data.edata.connected) {
            int colorMode = widget->getColorMode();
            if(colorMode == caPolyLine::Static) {
                // done at initialisation, we have to set it back after no connect
                if(!widget->property("Connect").value<bool>()) {
                    QColor fg = widget->property("FColor").value<QColor>();
                    widget->setForeground(fg);
                    widget->setLineColor(fg);
                    widget->setProperty("Connect", true);
                }
            } else if(colorMode == caPolyLine::Alarm) {
                int status = ComputeAlarm(w);
                widget->setAlarmColors(status);
            }

            // treat visibility if defined
            setObjectVisibility(widget, data.edata.rvalue);

        } else {
            widget->setAlarmColors(NOTCONNECTED);
            widget->setProperty("Connect", false);
        }

        // Led ==================================================================================================================
    } else if (caLed *widget = qobject_cast<caLed *>(w)) {
        //qDebug() << "led" << led->objectName();

        // todo define colors in caLed class
        if(data.edata.connected) {
            if(bitState((int) data.edata.ivalue, widget->getBitNr())) {
                widget->setState(true);
            } else {
                widget->setState(false);
            }
        } else {
            widget->setColor(QColor(Qt::white));
        }

        // ApplyNumeric and Numeric =====================================================================================================
    } else if (caApplyNumeric *widget = qobject_cast<caApplyNumeric *>(w)) {
        //qDebug() << "caApplyNumeric" << wheel->objectName() << kPtr->pv << data.edata.monitorCount;

        if(data.edata.connected) {
            ComputeNumericMaxMinPrec(widget, data);
            widget->setConnectedColors(true);
            widget->setValue(data.edata.rvalue);
            widget->setAccessW(data.edata.accessW);
        } else {
            widget->setConnectedColors(false);
        }

        // Numeric =====================================================================================================
    } else if (caNumeric *widget = qobject_cast<caNumeric *>(w)) {
        //qDebug() << "caNumeric" << widget->objectName() << data.pv;

        if(data.edata.connected) {
            ComputeNumericMaxMinPrec(widget, data);
            widget->setConnectedColors(true);
            widget->silentSetValue(data.edata.rvalue);
            widget->setAccessW(data.edata.accessW);
        } else {
            widget->setConnectedColors(false);
        }

        // cartesian plot ==================================================================================================================
    } else if (caCartesianPlot *widget = qobject_cast<caCartesianPlot *>(w)) {

        int curvNB = data.specData[0];
        int curvType = data.specData[1];  // Xonly, Yonly, XY_both; specData[2] is x or y
        int XorY = data.specData[2];      // X=0; Y=1;

        if(data.edata.connected) {
            // scale first time
            if(data.edata.initialize) {
                if(XorY == 0 && widget->getXscaling() == caCartesianPlot::Channel) {
                    if(data.edata.lower_disp_limit != data.edata.upper_disp_limit) {
                        widget->setScaleX(data.edata.lower_disp_limit, data.edata.upper_disp_limit);
                    } else {
                        widget->setXscaling(caCartesianPlot::Auto);
                    }
                } else if(XorY == 1 && widget->getYscaling() == caCartesianPlot::Channel) {
                    //qDebug() << "set scale to" << data.edata.lower_disp_limit << data.edata.upper_disp_limit;
                    if(data.edata.lower_disp_limit != data.edata.upper_disp_limit) {
                        widget->setScaleY(data.edata.lower_disp_limit, data.edata.upper_disp_limit);
                    } else {
                        widget->setYscaling(caCartesianPlot::Auto);
                    }
                }
            }

            if(!widget->property("Connect").value<bool>()) {
                widget->setProperty("Connect", true);
                widget->setAllProperties();
            }
            // data from y vector


            if(data.edata.valueCount > 0 && data.edata.dataB != (void*) 0) {
                QVector<double> y;
                y.clear();
                y.reserve(data.edata.valueCount);
                BuildVector(y);





                //if(y.size() > 0) {
                widget->setData(y, curvNB, curvType, XorY);
            } else {
                // data from value
                QVector<double> y1;
                y1.append(data.edata.rvalue);
                widget->setData(y1, curvNB, curvType, XorY);
            }
        } else {
            widget->setWhiteColors();
            widget->setProperty("Connect", false);
        }

        // stripchart ==================================================================================================================
    } else if(caStripPlot *widget = qobject_cast<caStripPlot *>(w)) {

        if(data.edata.connected) {
            double y0min = widget->getYaxisLimitsMin(0);
            double y0max = widget->getYaxisLimitsMax(0);
            int actPlot= data.specData[1];
            double ymin =  widget->getYaxisLimitsMin(actPlot);
            double ymax =  widget->getYaxisLimitsMax(actPlot);
            // we rescale this point to fit to the axis of the first plot
            double yp = (y0max - y0min) / (ymax -ymin) * (data.edata.rvalue - ymin) + y0min;
            widget->setData(yp, data.specData[1]);
        }

        // animated gif ==================================================================================================================
    } else if(caImage *widget = qobject_cast<caImage *>(w)) {

        double valueArray[MAX_CALC_INPUTS];
        char post[256];
        char calcString[256];
        long status;
        short errnum;
        double result;

        //qDebug() << "we have a image" << widget << kPtr->pv << data.edata.fieldtype << data.edata.dataSize << String;

        if(!data.edata.connected) {
            widget->setInvalid(Qt::white);
            return;
        } else {
            widget->setValid();
        }

        // treat visibility if defined, do nothing when not visible
        if(!setObjectVisibility(widget, data.edata.rvalue)) return; // do nothing when not visible

        // treat image frame number, if non dynamic image, do nothing
        if(widget->getFrameCount() <= 1) {
            return;
            // empty calc string, animation was already set
        } else if(widget->getImageCalc().size() == 0) {
            return;
        } else {
            // any monitors ?
            QVariant var=widget->property("MonitorList");
            QVariantList list = var.toList();
            int nbMonitors = list.at(0).toInt();
            //qDebug() << image << "number of monitors" << nbMonitors;
            if(nbMonitors > 0)  {

                // get calc string
                //printf("get calc string <%s>\n", qPrintable(widget->getImageCalc()));
                strcpy(calcString, (char*) widget->getImageCalc().toAscii().constData());

                // scan and get the channels
                for(int i=0; i < 4; i++) valueArray[i] = 0.0;
                for(int i=0; i< nbMonitors;i++) {
                    knobData *ptr = mutexKnobData->GetMutexKnobDataPtr(list.at(i+1).toInt());
                    //qDebug() << ptr->pv << ptr->edata.connected << ptr->edata.value;
                    // when connected
                    if(ptr->edata.connected) {
                        valueArray[i] = ptr->edata.rvalue;
                    } else {
                        valueArray[i] = 0.0;
                    }
                }

                status = postfix(calcString, post, &errnum);
                if(status) {
                    widget->setInvalid(Qt::black);
                    //qDebug() << "invalid calc 1" << calcString;
                }

                // Perform the calculation
                status = calcPerform(valueArray, &result, post);
                if(!status) {
                    // Result is valid, convert to frame number
                    if(result < 0.0) {
                        widget->setInvalid(Qt::black);
                        //printf("no valid frame\n");
                    } else {
                        //printf("frame ok=%d\n", (int)(result +.5));
                        widget->setFrame((int)(result +.5));
                    }
                } else {
                    //printf("invalid calc 2\n");
                    widget->setInvalid(Qt::black);
                }

                // no monitors
            } else {

            }
        }

        // table with pv name, value and unit==========================================================================
    } else if(caTable *widget = qobject_cast<caTable *>(w)) {

        //qDebug() << "table" << table->objectName() << kPtr->pv << data.edata.value << data.edata.connected;
        int row= data.specData[0];

        if(data.edata.connected) {
            // enum string
            if(data.edata.fieldtype == caENUM || data.edata.fieldtype == caSTRING) {
                int colorMode = widget->getColorMode();
                if(colorMode == caTable::Alarm) {
                    widget->displayText(row, 1, data.edata.severity, String);
                } else {
                    widget->displayText(row, 1, 0, String);
                }
                // double
            } else {
                if(data.edata.initialize) {
                    widget->setFormat(row, data.edata.precision);
                }
                widget->setValue(row, 1, data.edata.severity, data.edata.rvalue, data.edata.units);
            }

        } else {
            widget->displayText(row, 1, NOTCONNECTED, "");
            widget->displayText(row, 2, NOTCONNECTED, "");
        }

        // bitnames table with text and coloring according the value=========================================================
    } else if (caBitnames *widget = qobject_cast<caBitnames *>(w)) {

        if(data.edata.connected) {
            // set enum strings
            if(data.edata.fieldtype == caENUM) {
                widget->setEnumStrings(String);
            } else if(data.edata.fieldtype == caINT || data.edata.fieldtype == caLONG) {
                widget->setValue(data.edata.ivalue);
            }
        } else {
            // todo
        }

        // camera =========================================================
    } else if (caCamera *widget = qobject_cast<caCamera *>(w)) {

        //qDebug() << data.pv << data.edata.connected << data.specData[0];
        if(data.edata.connected) {
            if(data.specData[0] == 1) {        // width channel
                widget->setWidth((int) data.edata.rvalue);
            } else if(data.specData[0] == 2) { // height channel
                widget->setHeight((int) data.edata.rvalue);
            } else if(data.specData[0] == 3) { // code channel if present
                widget->setCode((int) data.edata.rvalue);
            } else if(data.specData[0] == 4) { // bpp channel if present
                widget->setBPP((int) data.edata.rvalue);
            } else if(data.specData[0] == 0) { // data channel
                widget->showImage(data.edata.dataSize, (char*) data.edata.dataB);
            }

        } else {
            // todo
        }

        // messagebutton, yust treat access ==========================================================================
    } else if (caMessageButton *widget = qobject_cast<caMessageButton *>(w)) {

        if(data.edata.connected) {
            widget->setAccessW(data.edata.accessW);
        }

        // something else (user defined monitors with non ca widgets ?) ==============================================
    } else {
        //qDebug() << "unrecognized widget" << w->metaObject()->className();
    }
}

/**
 * write value to device
 */
void CaQtDM_Lib::Callback_EApplyNumeric(double value)
{
    char errmess[255];
    caApplyNumeric *numeric = qobject_cast<caApplyNumeric *>(sender());
    if(numeric->getPV().length() > 0) {
        //qDebug() << numeric->objectName() << numeric->getPV() << value;
        EpicsSetValue((char*) numeric->getPV().toAscii().constData(), value, 0, "",
                      (char*) numeric->objectName().toLower().toAscii().constData(), errmess);
    }
}
/**
 * write value to device
 */
void CaQtDM_Lib::Callback_ENumeric(double value)
{
    char errmess[255];
    caNumeric *numeric = qobject_cast<caNumeric *>(sender());

    if(numeric->getPV().length() > 0) {
        //qDebug() << numeric->objectName() << numeric->getPV() << value;
        EpicsSetValue((char*) numeric->getPV().toAscii().constData(), value, 0, "",
                      (char*) numeric->objectName().toLower().toAscii().constData(), errmess);
    }
}

/**
 * write value to device
 */
void CaQtDM_Lib::Callback_Slider(double value)
{
    char errmess[255];
    caSlider *numeric = qobject_cast<caSlider *>(sender());

    if(numeric->getPV().length() > 0) {
        //qDebug() << numeric->objectName() << numeric->getPV() << value;
        EpicsSetValue((char*) numeric->getPV().toAscii().constData(), value, 0, "",
                      (char*) numeric->objectName().toLower().toAscii().constData(), errmess);
    }
}

/**
 * updates the text of a QLineEdit widget given by its name
 */
void CaQtDM_Lib::Callback_UpdateLine(const QString& text, const QString& name)
{
    //qDebug() << "Callback_UpdateLine" << text << name;
    QLineEdit *lineedit = this->findChild<QLineEdit*>(name);
    if (lineedit != NULL) lineedit->setText(text);
}

void CaQtDM_Lib::Callback_ChoiceClicked(const QString& text)
{
    char errmess[255];
    caChoice *choice = qobject_cast<caChoice *>(sender());
    QString pv = choice->getPV();
    //qDebug() << "choicecallback" << text << choice << choice->getPV();
    EpicsSetValue((char*) pv.toAscii().constData(), 0.0, 0, (char*) text.toAscii().constData(),
                  (char*) choice->objectName().toLower().toAscii().constData(), errmess);
}

void CaQtDM_Lib::Callback_MenuClicked(const QString& text)
{
    char errmess[255];
    caMenu *menu = qobject_cast<caMenu *>(sender());
    QString pv = menu->getPV();
    //qDebug() << "menu_clicked" << text << pv;
    EpicsSetValue((char*) pv.toAscii().constData(), 0.0, 0, (char*) text.toAscii().constData(),
                  (char*) menu->objectName().toLower().toAscii().constData(), errmess);
    // display label again when configured with it
    if(menu->getLabelDisplay()) {
        menu->setCurrentIndex(0);
    }
}

void CaQtDM_Lib::Callback_TextEntryChanged(const QString& text)
{
    char errmess[255];
    caTextEntry::FormatType fType;
    char textValue[40];
    double value;
    long longValue;
    char *end;
    bool match;

    strcpy(textValue, text.toAscii().constData());

    caTextEntry *w = qobject_cast<caTextEntry *>(sender());

    fType = w->getFormatType();

    int indx =  w->property("MonitorIndex").value<int>();

    if(indx < 0) return;

    knobData *kPtr = mutexKnobData->GetMutexKnobDataPtr(indx);  // use pointer for updating structure if necessary
    switch (kPtr->edata.fieldtype) {
    case caSTRING:
        EpicsSetValue(kPtr->pv, 0.0, 0, textValue, (char*) w->objectName().toLower().toAscii().constData(), errmess);
        break;

    case caENUM:
        // Check for a match
        match = false;
        if(kPtr->edata.dataB != (void*)0 && kPtr->edata.enumCount > 0) {
            QString strng((char*) kPtr->edata.dataB);
            QStringList list = strng.split(";", QString::SkipEmptyParts);
            for (int i=0; i<list.size(); i++) {
                if(!text.compare(list.at(i).trimmed())) {
                    EpicsSetValue((char*) kPtr->pv, 0.0, 0, textValue, (char*) w->objectName().toLower().toAscii().constData(), errmess);
                    match = true;
                    break;
                }
            }
        }

        if(!match) {
            // Assume it is a number
            if(fType == caTextEntry::octal) {
                longValue = strtoul(textValue, &end, 8);
            } else if(fType == caTextEntry::hexadecimal) {
                longValue = strtoul(textValue,&end,16);
            } else {
                if((strlen(textValue) > (size_t) 2) && (textValue[0] == '0') && (textValue[1] == 'x' || textValue[1] == 'X')) {
                    longValue = strtoul(textValue,&end,16);
                } else {
                    longValue = strtol(textValue,&end,10);
                }
            }
            if(*end == 0 && end != textValue && longValue >= 0 && longValue <= kPtr->edata.enumCount) {
                //qDebug() << "write the value" << longValue;
                EpicsSetValue((char*) kPtr->pv, 0.0, longValue, textValue, (char*) w->objectName().toLower().toAscii().constData(), errmess);
            } else {
                printf("caTextEntry: Invalid value: Name: %s  Value: \"%s\"\n", kPtr->pv, textValue);
            }
        }

        break;

    case caCHAR:
        if(fType == caTextEntry::string) {
            EpicsSetValue((char*) kPtr->pv, 0.0, 0, textValue, (char*) w->objectName().toLower().toAscii().constData(), errmess);
        }
        break;

    default:
        // Treat as a double
        if(fType == caTextEntry::octal) {
            value = (double) strtoul(textValue, &end, 8);
        } else if(fType == caTextEntry::hexadecimal) {
            value = (double) strtoul(textValue, &end, 16);
        } else {
            if((strlen(textValue) > (size_t) 2) && (textValue[0] == '0')
                    && (textValue[1] == 'x' || textValue[1] == 'X')) {
                value = (double)strtoul(textValue, &end, 16);
            } else {
                value = (double)strtod(textValue, &end);
            }
        }
        if(*end == '\0' && end != textValue) {
            EpicsSetValue((char*) kPtr->pv, value, 0, textValue, (char*) w->objectName().toLower().toAscii().constData(), errmess);
        } else {
            printf("caTextEntry: Invalid value: Name: %s  Value: \"%s\"\n", kPtr->pv, textValue);
        }
        break;
    }
}

void CaQtDM_Lib::Callback_RelatedDisplayClicked(int indx)
{
    caRelatedDisplay *w = qobject_cast<caRelatedDisplay *>(sender());
    //qDebug() << "relateddisplaycallback" << indx << w;
    QStringList files = w->getFiles().split(";", QString::SkipEmptyParts);
    QStringList args = w->getArgs().split(";", QString::SkipEmptyParts);
    //qDebug() << "files:" << files;
    //qDebug() << "args" << args;
    if(indx < files.count() && indx < args.count()) {
        emit Signal_OpenNewWFile(files[indx].trimmed(), args[indx].trimmed());
    } else if(indx < files.count()) {
        emit Signal_OpenNewWFile(files[indx].trimmed(), "");
    }
}

void CaQtDM_Lib::Callback_MessageButton(int type)
{
    char errmess[255];
    caMessageButton *w = qobject_cast<caMessageButton *>(sender());
    if(type == 0) {         // pressed
        if(w->getPressMessage().size() > 0)
            EpicsSetValue((char*) w->getPV().toAscii().constData(), 0.0, 0,
                          (char*) w->getPressMessage().toAscii().constData(),
                          (char*) w->objectName().toLower().toAscii().constData(), errmess);
    } else if(type == 1) {  // released
        if(w->getReleaseMessage().size() > 0)
            EpicsSetValue((char*) w->getPV().toAscii().constData(), 0.0, 0,
                          (char*) w->getReleaseMessage().toAscii().constData(),
                          (char*) w->objectName().toLower().toAscii().constData(), errmess);
    }
    //qDebug() << "messagebutton pressed" << w << type;
}

void CaQtDM_Lib::Callback_ShellCommandClicked(int indx)
{
    proc = new QProcess( this);
    proc->setWorkingDirectory(".");
    proc->setProcessChannelMode(QProcess::MergedChannels);
    QObject::connect( proc, SIGNAL(error(QProcess::ProcessError)),
                      this, SLOT(processError(QProcess::ProcessError)));

    caShellCommand *choice = qobject_cast<caShellCommand *>(sender());
    QStringList commands = choice->getFiles().split(";");
    QStringList args = choice->getArgs().split(";");
    if(indx < commands.count() && indx < args.count()) {
        QString command;
        command.append(commands[indx].trimmed());
        command.append(" ");
        command.append(args[indx].trimmed());
        printf("execute command= <%s>\n", qPrintable(command.trimmed()));
        // replace medm by caQtDM
        command.replace("camedm ", "caQtDM ");
        command.replace("piomedm ", "caQtDM ");
        command.replace("medm ", "caQtDM ");
        postMessage(QtDebugMsg, (char*) qPrintable(command.trimmed()));
        //printf("execute command= <%s>\n", qPrintable(command.trimmed()));
        proc->start(command.trimmed(),QIODevice::ReadWrite);
    } else if(indx < commands.count()) {
        QString command;
        command.append(commands[indx].trimmed());
        //printf("execute command= <%s>\n", qPrintable(command.trimmed()));
        postMessage(QtDebugMsg, (char*) qPrintable(command.trimmed()));
        proc->start(command.trimmed(),QIODevice::ReadWrite);
    }

    // read output, but blocks application until child exits
    /*
    QByteArray data;
    while(proc->waitForReadyRead())
        data.append(proc->readAll());
    // Output the data
    qDebug(data.data());
    */
}

void CaQtDM_Lib::processError(QProcess::ProcessError err)
{
    switch(err)
    {
    case QProcess::FailedToStart:
        QMessageBox::information(0,"FailedToStart","FailedToStart");
        break;
    case QProcess::Crashed:
        QMessageBox::information(0,"Crashed","Crashed");
        break;
    case QProcess::Timedout:
        QMessageBox::information(0,"FailedToStart","FailedToStart");
        break;
    case QProcess::WriteError:
        QMessageBox::information(0,"Timedout","Timedout");
        break;
    case QProcess::ReadError:
        QMessageBox::information(0,"ReadError","ReadError");
        break;
    case QProcess::UnknownError:
        QMessageBox::information(0,"UnknownError","UnknownError");
        break;
    default:
        QMessageBox::information(0,"default","default");
        break;
    }
}

void CaQtDM_Lib::closeEvent(QCloseEvent* ce)
{
    for(int i=0; i < mutexKnobData->GetMutexKnobDataSize(); i++) {
        knobData *kPtr = mutexKnobData->GetMutexKnobDataPtr(i);
        if((kPtr->index != -1) && (myWidget == kPtr->thisW)) {
            ClearMonitor(kPtr);
        }
    }
}

void CaQtDM_Lib::DisplayContextMenu(QWidget* w)
{
    QMenu myMenu;
    QPoint pos =QCursor::pos() ;

    QString pv[20];
    int nbPV = 1;
    int limitsDefault = false;
    int precMode = false;
    int limitsMode = false;
    int Precision = 0;
    char *caTypeStr[] = {"DBF_STRING", "DBF_INT", "DBF_FLOAT", "DBF_ENUM", "DBF_CHAR", "DBF_LONG", "DBF_DOUBLE"};
    char colMode[20] = {""};
    double limitsMax=0.0, limitsMin=0.0;

    if(caImage* widget = qobject_cast<caImage *>(w)) {
        pv[0] = widget->getChannelA().trimmed();
        pv[1] = widget->getChannelB().trimmed();
        pv[2] = widget->getChannelC().trimmed();
        pv[3] = widget->getChannelB().trimmed();
        nbPV = 4;
    } else if(caMenu* widget = qobject_cast<caMenu *>(w)) {
        pv[0] = widget->getPV().trimmed();
    } else if(caChoice* widget = qobject_cast<caChoice *>(w)) {
        pv[0] = widget->getPV().trimmed();
        if(widget->getColorMode() == caChoice::Alarm) strcpy(colMode, "Alarm");
        else strcpy(colMode, "Static");
    } else if(caTextEntry* widget = qobject_cast<caTextEntry *>(w)) {
        pv[0] = widget->getPV().trimmed();
    } else if(caLineEdit* widget = qobject_cast<caLineEdit *>(w)) {
        pv[0] = widget->getPV().trimmed();
        if(widget->getPrecisionMode() == caLineEdit::User) {
            precMode = true;
            Precision = widget->getPrecision();
        }
        if(widget->getLimitsMode() == caLineEdit::User) {
            limitsMode = true;
            limitsMax = widget->getMaxValue();
            limitsMin = widget->getMinValue();
        }
        if(widget->getColorMode() == caLineEdit::Alarm_Default) strcpy(colMode, "Alarm");
        else if(widget->getColorMode() == caLineEdit::Alarm_Static) strcpy(colMode, "Alarm");
        else strcpy(colMode, "Static");

    } else if(caGraphics* widget = qobject_cast<caGraphics *>(w)) {
        pv[0] = widget->getChannelA().trimmed();
        pv[1] = widget->getChannelB().trimmed();
        pv[2] = widget->getChannelC().trimmed();
        pv[3] = widget->getChannelB().trimmed();
        nbPV = 4;
        if(widget->getColorMode() == caGraphics::Alarm) strcpy(colMode, "Alarm");
        else strcpy(colMode, "Static");
    } else if(caPolyLine* widget = qobject_cast<caPolyLine *>(w)) {
        pv[0] = widget->getChannelA().trimmed();
        pv[1] = widget->getChannelB().trimmed();
        pv[2] = widget->getChannelC().trimmed();
        pv[3] = widget->getChannelB().trimmed();
        nbPV = 4;
        if(widget->getColorMode() == caPolyLine::Alarm) strcpy(colMode, "Alarm");
        else strcpy(colMode, "Static");
    } else if (caApplyNumeric* widget = qobject_cast<caApplyNumeric *>(w)) {
        pv[0] = widget->getPV().trimmed();
    } else if (caNumeric* widget = qobject_cast<caNumeric *>(w)) {
        pv[0] = widget->getPV();
    } else if (caMessageButton* widget = qobject_cast<caMessageButton *>(w)) {
        pv[0] = widget->getPV().trimmed();
    } else if(caLed* widget = qobject_cast<caLed *>(w)) {
        pv[0] = widget->getPV().trimmed();
    } else if(caBitnames* widget = qobject_cast<caBitnames *>(w)) {
        pv[0] = widget->getEnumPV().trimmed();
        pv[1] = widget->getValuePV().trimmed();
        nbPV = 2;
    } else if(caSlider* widget = qobject_cast<caSlider *>(w)) {
        pv[0] = widget->getPV().trimmed();
        if(widget->getLimitsMode() == caSlider::User) {
            limitsMode = true;
            limitsMax = widget->getMaxValue();
            limitsMin = widget->getMinValue();
        }
        if(widget->getColorMode() == caSlider::Alarm) strcpy(colMode, "Alarm");
        else strcpy(colMode, "Static");
    } else if(caThermo* widget = qobject_cast<caThermo *>(w)) {
        pv[0] = widget->getPV().trimmed();
        if(widget->getLimitsMode() == caThermo::User) {
            limitsMode = true;
            limitsMax = widget->maxValue();
            limitsMin = widget->minValue();
        }
        knobData *kPtr = mutexKnobData->getMutexKnobDataPV(pv[0]);
        if(kPtr->edata.lower_disp_limit == kPtr->edata.upper_disp_limit) {
            limitsDefault = true;
            limitsMax = widget->maxValue();
            limitsMin = widget->minValue();
        }
        if(widget->getColorMode() == caThermo::Alarm) strcpy(colMode, "Alarm");
        else strcpy(colMode, "Static");
    } else if(caLinearGauge* widget = qobject_cast<caLinearGauge *>(w)) {
        pv[0] = widget->getPV().trimmed();
    } else if(caByte* widget = qobject_cast<caByte *>(w)) {
        pv[0] = widget->getPV().trimmed();
        if(widget->getColorMode() == caByte::Alarm) strcpy(colMode, "Alarm");
        else strcpy(colMode, "Static");
    } else if(caStripPlot* widget = qobject_cast<caStripPlot *>(w)) {
        QString pvs = widget->getPVS();
        QStringList vars = pvs.split(";", QString::SkipEmptyParts);
        nbPV = min(vars.count(), 5);
        for(int i=0; i<nbPV; i++) {
            pv[i] = vars.at(i).trimmed();
        }
    } else if(caCartesianPlot* widget = qobject_cast<caCartesianPlot *>(w)) {
        nbPV = 0;
        for(int i=0; i< 4; i++) {
            QStringList thisString;
            if(i==0) thisString = widget->getPV_1().split(";");
            if(i==1) thisString = widget->getPV_2().split(";");
            if(i==2) thisString = widget->getPV_3().split(";");
            if(i==3) thisString = widget->getPV_4().split(";");
            if(thisString.count() == 2 && thisString.at(0).trimmed().length() > 0 && thisString.at(1).trimmed().length() > 0) {
                pv[nbPV++] = thisString.at(0).trimmed();
                pv[nbPV++] = thisString.at(1).trimmed();
            } else if(thisString.count() == 2 && thisString.at(0).trimmed().length() > 0 && thisString.at(1).trimmed().length() == 0) {
                pv[nbPV++] = thisString.at(0).trimmed();
            } else if(thisString.count() == 2 && thisString.at(1).trimmed().length() > 0 && thisString.at(0).trimmed().length() == 0) {
                pv[nbPV++] = thisString.at(1).trimmed();
            }
        }
    } else if(caCamera* widget = qobject_cast<caCamera *>(w)) {
        nbPV=0;
        for(int i=0; i< 5; i++) {
            QString text;
            if(i==0) text = widget->getPV_Data();
            if(i==1) text = widget->getPV_Width();
            if(i==2) text = widget->getPV_Height();
            if(i==3) text = widget->getPV_Code();
            if(i==4) text = widget->getPV_BPP();
            if(text.size() > 0) {
                pv[nbPV] = text;
                nbPV++;
            }
        }
    } else  {
        return;
    }

    // construct info for the pv we are pointing at
    myMenu.addAction("Get Info");
    myMenu.addAction("Print");

    QAction* selectedItem = myMenu.exec(pos);
    if (selectedItem) {
        //qDebug() << "item selected=" << selectedItem << selectedItem->text();
        if(selectedItem->text().contains("Get Info")) {
            QString info;
            info.append(InfoPrefix);
            info.append("-------------------------------------------------<br>");
            info.append("Object: ");
            info.append(w->objectName());
            info.append("<br>");
            for(int i=0; i< nbPV; i++) {
                knobData *kPtr = mutexKnobData->getMutexKnobDataPV(pv[i]);  // use pointer for getting all necessary information
                if((kPtr != (knobData*) 0) && (pv[i].length() > 0)) {
                    char asc[80];
                    info.append("<br>");
                    info.append(kPtr->pv);

                    connectInfo *tmp = (connectInfo *) kPtr->edata.info;
                    if (tmp != (connectInfo *) 0) {
                        if(tmp->connected) {
                            info.append(" : connected");
                        } else {
                            if(tmp->cs != 1) { // epics
                                info.append(" : not connected");
                            } else {
                                info.append(" : acs device");
                            }
                        }
                    }

                    info.append("<br>=========================<br>Type: ");
                    info.append(caTypeStr[kPtr->edata.fieldtype]);

                    sprintf(asc,"<br>Count: %d", kPtr->edata.valueCount);
                    info.append(asc);

                    info.append("<br>Value: ");
                    switch (kPtr->edata.fieldtype) {
                    case caCHAR:
                        break;
                    case caSTRING:
                        info.append((char*)kPtr->edata.dataB);
                        break;

                    case caENUM:{
                        sprintf(asc,"%ld %s", kPtr->edata.ivalue, kPtr->edata.units);
                        info.append(asc);
                        sprintf(asc,"<br>nbStates: %d", kPtr->edata.enumCount);
                        info.append(asc);
                        info.append("<br>States: ");
                        QString States((char*) kPtr->edata.dataB);
                        QStringList list = States.split(";", QString::SkipEmptyParts);
                        for(int i=0; i<list.count(); i++) {
                            sprintf(asc, "<br>&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;%d %s", i, qPrintable(list.at(i)));
                            info.append(asc);
                        }
                        break;
                    }
                    case caINT:
                    case caLONG:
                        sprintf(asc,"%ld %s", kPtr->edata.ivalue, kPtr->edata.units);
                        info.append(asc);
                        break;
                    case caFLOAT:
                    case caDOUBLE:
                        sprintf(asc,"%lf %s", kPtr->edata.rvalue, kPtr->edata.units);
                        info.append(asc);
                        break;
                    }

                    // severity
                    switch(kPtr->edata.severity) {
                    case NO_ALARM:
                        info.append("<br>Severity: NO_ALARM");
                        break;
                    case MINOR_ALARM:
                        info.append("<br>Severity: MINOR_ALARM");
                        break;
                    case MAJOR_ALARM:
                        info.append("<br>Severity: MAJOR_ALARM");
                        break;
                    case INVALID_ALARM:
                        info.append("<br>Severity: INVALID_ALARM");
                        break;
                    case NOTCONNECTED:
                        info.append("<br>Severity: NOT_CONNECTED");
                        break;
                    }

                    // ioc
                    info.append("<br>IOC: ");
                    info.append(kPtr->edata.fec);

                    // precision
                    if(!precMode) {
                        sprintf(asc,"<br>Precision (channel) :%d ", kPtr->edata.precision);
                        Precision = kPtr->edata.precision;
                    } else {
                        sprintf(asc,"<br>Precision (user) :%d ", Precision);
                    }
                    info.append(asc);

                    // limits
                    if(limitsMode) {
                        sprintf(asc,"<br>User alarm: MIN:%.*f  MAX:%.*f ", Precision, limitsMin, Precision, limitsMax);
                        info.append(asc);
                    }
                    if(limitsDefault) {
                        sprintf(asc,"<br>Default limits: MIN:%.*f  MAX:%.*f ", Precision, limitsMin, Precision, limitsMax);
                        info.append(asc);
                    }
                    sprintf(asc,"<br>LOPR:%.*f  HOPR:%.*f ", Precision, kPtr->edata.lower_disp_limit,
                            Precision, kPtr->edata.upper_disp_limit);
                    info.append(asc);
                    sprintf(asc,"<br>LOLO:%.*f  HIHI:%.*f ", Precision,kPtr->edata.lower_alarm_limit,
                            Precision, kPtr->edata.upper_alarm_limit);
                    info.append(asc);
                    sprintf(asc,"<br>LOW :%.*f  HIGH:%.*f ", Precision, kPtr->edata.lower_warning_limit,
                            Precision, kPtr->edata.upper_warning_limit);
                    info.append(asc);
                    sprintf(asc,"<br>DRVL:%.*f  DRVH:%.*f ", Precision,kPtr->edata.lower_ctrl_limit,
                            Precision, kPtr->edata.upper_ctrl_limit);
                    info.append(asc);
                    info.append("<br>");

                    //colormode
                    if(strlen(colMode) > 0) {
                        sprintf(asc, "Colormode: %s", colMode);
                        info.append(asc);
                        info.append("<br>");
                    }

                    // access
                    if(kPtr->edata.accessR==1 && kPtr->edata.accessW==1) sprintf(asc, "Access: ReadWrite");
                    else if(kPtr->edata.accessR==1 && kPtr->edata.accessW==0) sprintf(asc, "Access: ReadOnly");
                    else if(kPtr->edata.accessR==0 && kPtr->edata.accessW==1) sprintf(asc, "Access: WriteOnly"); // not possible
                    else sprintf(asc, "Access: NoAccess");
                    info.append(asc);
                    info.append("<br>");
                }
            }
            info.append(InfoPostfix);

            //QMessageBox::information(this,"pv info",info);

            QMessageBox box(QMessageBox::Information, "pv info", info, QMessageBox::Close);
            const QObjectList children = box.children();
            QGridLayout *layout = qobject_cast<QGridLayout *>(children.at(0));
            QLayoutItem *item = layout->itemAtPosition(0, 1);
            if (item) {
                QWidget *widget = item->widget();
                if (widget) {
                    qobject_cast<QLabel *>(widget)->setWordWrap(true);
                    layout->removeWidget(widget);
                    QScrollArea *area = new QScrollArea();
                    area->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
                    area->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
                    area->setMinimumHeight(500);
                    area->setMinimumWidth(300);
                    area->setWidget(widget);
                    layout->addWidget(area, 0, 1);
                }
            }

            box.exec();


        } else if(selectedItem->text().contains("Print")) {
            print();
        }
    } else {
        // nothing was chosen
    }
}

void CaQtDM_Lib::ShowContextMenu(const QPoint& position) // this is a slot
{
    DisplayContextMenu(qobject_cast<QWidget *>(sender()));
}

void CaQtDM_Lib::mouseReleaseEvent(QMouseEvent *event)
{
    emit customContextMenuRequested(QPoint(event->x(),event->y()));
}

void CaQtDM_Lib::InitVisibility(QWidget* widget, knobData* kData, QMap<QString, QString> map,  int *specData)
{
    QString tooltip;
    QString pv;
    bool doNothing;

    tooltip.append(ToolTipPrefix);
    QList<QVariant> integerList;
    int num, nbMon = 0;
    QString strng[4];
    QString visibilityCalc;

    if (caImage *w = qobject_cast<caImage *>(widget)) {
        strng[0] = w->getChannelA();
        strng[1] = w->getChannelB();
        strng[2] = w->getChannelC();
        strng[3] = w->getChannelD();
        visibilityCalc = w->getVisibilityCalc();
    } else if (caGraphics *w = qobject_cast<caGraphics *>(widget)) {
        strng[0] = w->getChannelA();
        strng[1] = w->getChannelB();
        strng[2] = w->getChannelC();
        strng[3] = w->getChannelD();
        visibilityCalc = w->getVisibilityCalc();
    } else if (caPolyLine *w = qobject_cast<caPolyLine *>(widget)) {
        strng[0] = w->getChannelA();
        strng[1] = w->getChannelB();
        strng[2] = w->getChannelC();
        strng[3] = w->getChannelD();
        visibilityCalc = w->getVisibilityCalc();
    } else if (caInclude *w = qobject_cast<caInclude *>(widget)) {
        strng[0] = w->getChannelA();
        strng[1] = w->getChannelB();
        strng[2] = w->getChannelC();
        strng[3] = w->getChannelD();
        visibilityCalc = w->getVisibilityCalc();
    } else if (caFrame *w = qobject_cast<caFrame *>(widget)) {
        strng[0] = w->getChannelA();
        strng[1] = w->getChannelB();
        strng[2] = w->getChannelC();
        strng[3] = w->getChannelD();
        visibilityCalc = w->getVisibilityCalc();
    } else if (caLabel *w = qobject_cast<caLabel *>(widget)) {
        strng[0] = w->getChannelA();
        strng[1] = w->getChannelB();
        strng[2] = w->getChannelC();
        strng[3] = w->getChannelD();
        visibilityCalc = w->getVisibilityCalc();
    } else {
        qDebug() << "widget has not been defined for visibility";
        return;
    }

    /* add monitors for this image if any */
    for(int i=0; i<4; i++) {
        /* add monitors for this image if any */
        if((num = addMonitor(myWidget, kData, strng[i], widget, specData, map, &pv)) >= 0) {

            if (caImage *w = qobject_cast<caImage *>(widget)) {
                if(i==0) w->setChannelA(pv);  /* replace pv while macro could be used */
                if(i==1) w->setChannelB(pv);  /* replace pv while macro could be used */
                if(i==2) w->setChannelC(pv);  /* replace pv while macro could be used */
                if(i==3) w->setChannelD(pv);  /* replace pv while macro could be used */
            } else if (caGraphics *w = qobject_cast<caGraphics *>(widget)) {
                if(i==0) w->setChannelA(pv);  /* replace pv while macro could be used */
                if(i==1) w->setChannelB(pv);  /* replace pv while macro could be used */
                if(i==2) w->setChannelC(pv);  /* replace pv while macro could be used */
                if(i==3) w->setChannelD(pv);  /* replace pv while macro could be used */
            } else if (caPolyLine *w = qobject_cast<caPolyLine *>(widget)) {
                if(i==0) w->setChannelA(pv);  /* replace pv while macro could be used */
                if(i==1) w->setChannelB(pv);  /* replace pv while macro could be used */
                if(i==2) w->setChannelC(pv);  /* replace pv while macro could be used */
                if(i==3) w->setChannelD(pv);  /* replace pv while macro could be used */
            } else if (caInclude *w = qobject_cast<caInclude *>(widget)) {
                if(i==0) w->setChannelA(pv);  /* replace pv while macro could be used */
                if(i==1) w->setChannelB(pv);  /* replace pv while macro could be used */
                if(i==2) w->setChannelC(pv);  /* replace pv while macro could be used */
                if(i==3) w->setChannelD(pv);  /* replace pv while macro could be used */
            } else if (caFrame *w = qobject_cast<caFrame *>(widget)) {
                if(i==0) w->setChannelA(pv);  /* replace pv while macro could be used */
                if(i==1) w->setChannelB(pv);  /* replace pv while macro could be used */
                if(i==2) w->setChannelC(pv);  /* replace pv while macro could be used */
                if(i==3) w->setChannelD(pv);  /* replace pv while macro could be used */
            } else if (caLabel *w = qobject_cast<caLabel *>(widget)) {
                if(i==0) w->setChannelA(pv);  /* replace pv while macro could be used */
                if(i==1) w->setChannelB(pv);  /* replace pv while macro could be used */
                if(i==2) w->setChannelC(pv);  /* replace pv while macro could be used */
                if(i==3) w->setChannelD(pv);  /* replace pv while macro could be used */
            }

            tooltip.append(pv);
            tooltip.append("<br>");
            integerList.append(num);
            nbMon++;
        }
    }
    tooltip.append(ToolTipPostfix);
    /* replace also some macro values in the visibility calc string */
    QString text =  treatMacro(map, visibilityCalc, &doNothing);

    if (caImage *w = qobject_cast<caImage *>(widget)) {
        text =  treatMacro(map, w->getImageCalc(), &doNothing);
        w->setImageCalc(text);
    }

    integerList.insert(0, nbMon); /* set property into widget */

    if (caImage *w = qobject_cast<caImage *>(widget)) {
        w->setVisibilityCalc(text);
        w->setProperty("MonitorList", integerList);
    } else if (caGraphics *w = qobject_cast<caGraphics *>(widget)) {
        w->setVisibilityCalc(text);
        w->setProperty("MonitorList", integerList);
    } else if (caPolyLine *w = qobject_cast<caPolyLine *>(widget)) {
        w->setVisibilityCalc(text);
        w->setProperty("MonitorList", integerList);
    } else if (caInclude *w = qobject_cast<caInclude *>(widget)) {
        w->setVisibilityCalc(text);
        w->setProperty("MonitorList", integerList);
    } else if (caFrame *w = qobject_cast<caFrame *>(widget)) {
        w->setVisibilityCalc(text);
        w->setProperty("MonitorList", integerList);
    } else if (caLabel *w = qobject_cast<caLabel *>(widget)) {
        w->setVisibilityCalc(text);
        w->setProperty("MonitorList", integerList);
    }
}

void CaQtDM_Lib::ComputeNumericMaxMinPrec(QWidget* widget, const knobData& data)
{
    double maxValue = 1.0, minValue = 0.0;
    int precMode=0, limitsMode=0;
    int width, prec, caMode;
    float maxAbsHoprLopr = 0.0;
    bool fixedFormat = false;

    if(data.edata.initialize) {

        if (caApplyNumeric *w = qobject_cast<caApplyNumeric *>(widget)) {
            precMode = w->getPrecisionMode();
            limitsMode = w->getLimitsMode();
            fixedFormat = w->getFixedFormat();
            caMode = caApplyNumeric::Channel;
        } else if (caNumeric *w = qobject_cast<caNumeric *>(widget)) {
            precMode = w->getPrecisionMode();
            limitsMode = w->getLimitsMode();
            fixedFormat = w->getFixedFormat();
            caMode = caNumeric::Channel;
        }
        if(limitsMode == caMode) {
            if((data.edata.upper_disp_limit == data.edata.lower_disp_limit) ||
                    (fabs(data.edata.upper_disp_limit - data.edata.lower_disp_limit) <= 0.001)) {
                maxValue = 100000.0;
                minValue = -100000.0;
            } else {
                maxValue = data.edata.upper_disp_limit;
                minValue = data.edata.lower_disp_limit;
            }
        } else {
            if (caApplyNumeric *w = qobject_cast<caApplyNumeric *>(widget)) {
                maxValue = w->getMaxValue();
                minValue = w->getMinValue();
            } else if (caNumeric *w = qobject_cast<caNumeric *>(widget)) {
                maxValue = w->getMaxValue();
                minValue = w->getMinValue();
            }
        }

        if (caApplyNumeric *w = qobject_cast<caApplyNumeric *>(widget)) {
            w->setMaxValue(maxValue);
            w->setMinValue(minValue);
        } else if (caNumeric *w = qobject_cast<caNumeric *>(widget)) {
            w->setMaxValue(maxValue);
            w->setMinValue(minValue);
        }
        if(!fixedFormat) {
            if(precMode == caMode) {
                prec = data.edata.precision;
                if(prec < 0) prec = 0;
                if(prec > 4) prec = 4;
                maxAbsHoprLopr= qMax(fabs(maxValue), fabs(minValue));
                if(maxAbsHoprLopr > 1.0) {
                    width = (int)log10(maxAbsHoprLopr) + 2 + prec;
                } else {
                    width = 2 + prec;
                }

                if (caApplyNumeric *w = qobject_cast<caApplyNumeric *>(widget)) {
                    w->setIntDigits(width-prec-1);
                    w->setDecDigits(prec);
                } else if (caNumeric *w = qobject_cast<caNumeric *>(widget)) {
                    w->setIntDigits(width-prec-1);
                    w->setDecDigits(prec);
                }
            } else {
                float maxAbsHoprLopr= qMax(fabs(maxValue), fabs(minValue));
                if(maxAbsHoprLopr > 1.0) {
                    width = (int)log10(maxAbsHoprLopr) + 2 ;
                } else {
                    width = 2;
                }
                if (caApplyNumeric *w = qobject_cast<caApplyNumeric *>(widget)) {
                    w->setIntDigits(width-1);
                } else if (caNumeric *w = qobject_cast<caNumeric *>(widget)) {
                    w->setIntDigits(width-1);
                }
            }
        }
    }
}

void CaQtDM_Lib::postMessage(QtMsgType type, char *msg)
{
    if(messageWindow == 0) return;
    messageWindow->postMsgEvent(type, msg);
}
