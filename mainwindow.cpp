#include "mainwindow.h"
#include "ui_mainwindow.h"

MainWindow::MainWindow(QWidget *parent) :
    QMainWindow(parent),
    ui(new Ui::MainWindow)
{
    ui->setupUi(this);
}

MainWindow::~MainWindow()
{
    Stop();
    Wait(10*1000); // 10 secs
    mLoopRcv.reset();
    delete ui;
}

void MainWindow::Init(const QString &savedir, const QString& fileextension, const unsigned int port, const long eostudy_timeout) {
    mSaveDir = savedir;

    if(!mLoopRcv)
        mLoopRcv = std::make_unique<CineLoopRcv>(mSaveDir, fileextension, port, eostudy_timeout, true, this);

    mLoopRcv->init();
    connect(mLoopRcv.get(), SIGNAL(finished()), mLoopRcv.get(), SLOT(deleteLater()));
    connect(mLoopRcv.get(), SIGNAL(dcmFileReceived(const QString&)),this, SLOT(showModifiedFile(const QString&)));
}

void MainWindow::Start() {
    if(mLoopRcv) mLoopRcv->start();
}

void MainWindow::Stop() {
    if(mLoopRcv) mLoopRcv->stop();
}

void MainWindow::Wait(unsigned long time_in_milliseconds) {
    if(mLoopRcv) mLoopRcv->wait(time_in_milliseconds);
}
