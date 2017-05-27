#pragma once

#include "dcmtk/config/osconfig.h"    /* make sure OS specific configuration is included first */

#include "dcmtk/ofstd/ofstdinc.h"

#include "dcmtk/ofstd/ofstd.h"
#include "dcmtk/ofstd/ofconapp.h"
#include "dcmtk/ofstd/ofdatime.h"
#include "dcmtk/dcmnet/dicom.h"         /* for DICOM_APPLICATION_ACCEPTOR */
#include "dcmtk/dcmnet/dimse.h"
#include "dcmtk/dcmnet/diutil.h"
#include "dcmtk/dcmnet/dcasccfg.h"      /* for class DcmAssociationConfiguration */
#include "dcmtk/dcmnet/dcasccff.h"      /* for class DcmAssociationConfigurationFile */
#include "dcmtk/dcmdata/dcfilefo.h"
#include "dcmtk/dcmdata/dcuid.h"
#include "dcmtk/dcmdata/dcdict.h"
#include "dcmtk/dcmdata/cmdlnarg.h"
#include "dcmtk/dcmdata/dcmetinf.h"
#include "dcmtk/dcmdata/dcuid.h"        /* for dcmtk version name */
#include "dcmtk/dcmdata/dcdeftag.h"
#include "dcmtk/dcmdata/dcostrmz.h"     /* for dcmZlibCompressionLevel */

//#ifdef WITH_OPENSSL
#include "dcmtk/dcmtls/tlstrans.h"
#include "dcmtk/dcmtls/tlslayer.h"
//#endif

#include <QMutex>
#include <QThread>


#define OFFIS_CONSOLE_APPLICATION "xrfviewer"

static OFLogger storescpLogger = OFLog::getLogger("dcmtk.apps." OFFIS_CONSOLE_APPLICATION);

static char rcsid[] = "$dcmtk: " OFFIS_CONSOLE_APPLICATION " v" OFFIS_DCMTK_VERSION " " OFFIS_DCMTK_RELEASEDATE " $";

#define APPLICATIONTITLE "crtvision"     /* our application entity title */

#define PATH_PLACEHOLDER "#p"
#define FILENAME_PLACEHOLDER "#f"
#define CALLING_AETITLE_PLACEHOLDER "#a"
#define CALLED_AETITLE_PLACEHOLDER "#c"
#define CALLING_PRESENTATION_ADDRESS_PLACEHOLDER "#r"


class CineLoopRcv : public QThread
{
    Q_OBJECT
public:
    explicit CineLoopRcv(const QString& outdir, const QString& fileextension, unsigned int port, long eostudy_timeout = -1, bool promiscuous = false, QObject *parent = 0);

    ~CineLoopRcv();

    bool init();

    void run() Q_DECL_OVERRIDE;

    void emitDcmFileReceivedSignal(const QString& fullpath);

    OFCondition acceptAssociation();

    OFBool            ignore()              { return opt_ignore; }
    OFBool            usemetaheader()       { return opt_useMetaheader; }
    T_ASC_Network*    netobj()                 { return net; }
    E_GrpLenEncoding  grouplength()         { return opt_groupLength; }
    E_EncodingType    sequencetype()        { return opt_sequenceType; }
    E_PaddingEncoding paddingtype()         { return opt_paddingType; }
    OFCmdUnsignedInt  filepad()             { return opt_filepad; }
    OFCmdUnsignedInt  itempad()             { return opt_itempad; }
    OFList<OFString>& outputfilenamearray() { return outputFileNameArray; }
    E_TransferSyntax& writetransfersyntax() { return opt_writeTransferSyntax; }

signals:
    void dcmFileReceived(const QString& fullpath);

public slots:
    void stop();

protected:
    OFCondition& cleanup();
    OFCondition processCommands();
    OFCondition echoSCP();
    OFCondition storeSCP();
    DUL_PRESENTATIONCONTEXT * findPresentationContextID(LST_HEAD * head, T_ASC_PresentationContextID presentationContextID);
    OFCondition acceptUnknownContextsWithTransferSyntax(T_ASC_Parameters * params, const char* transferSyntax, T_ASC_SC_ROLE acceptedRole);
    OFCondition acceptUnknownContextsWithPreferredTransferSyntaxes(T_ASC_Parameters * params,
                                                                   const char* transferSyntaxes[],
                                                                   int transferSyntaxCount,
                                                                   T_ASC_SC_ROLE acceptedRole = ASC_SC_ROLE_DEFAULT);
private:
    QMutex mutex;
    bool stopRunning;

    OFCondition cond;

    T_ASC_Network *net;
    DcmAssociationConfiguration asccfg;
    T_ASC_Association *assoc;

    OFString           opt_fileNameExtension;
    OFCmdUnsignedInt   opt_port;
    OFCmdUnsignedInt   opt_maxPDU;
    OFBool             opt_useMetaheader;
    E_TransferSyntax   opt_networkTransferSyntax;
    E_TransferSyntax   opt_writeTransferSyntax;
    E_GrpLenEncoding   opt_groupLength;
    E_EncodingType     opt_sequenceType;
    E_PaddingEncoding  opt_paddingType;
    OFCmdUnsignedInt   opt_filepad;
    OFCmdUnsignedInt   opt_itempad;
    OFBool             opt_ignore;
    OFBool             opt_promiscuous;
    OFString           callingAETitle;                    // calling application entity title will be stored here
    OFString           lastCallingAETitle;
    OFString           calledAETitle;                     // called application entity title will be stored here
    OFString           lastCalledAETitle;
    OFString           callingPresentationAddress;        // remote hostname or IP address will be stored here
    OFString           lastCallingPresentationAddress;
    const char *       opt_respondingAETitle;
    OFString           opt_outputDirectory;         // default: output directory equals "."
    OFString           lastStudyInstanceUID;
    OFString           subdirectoryPathAndName;
    OFList<OFString>   outputFileNameArray;
    OFString           lastStudySubdirectoryPathAndName;
    long               opt_endOfStudyTimeout;        // default: no end of study timeout
    T_DIMSE_BlockingMode opt_blockMode;
    int                opt_dimse_timeout;
    int                opt_acse_timeout;

    T_DIMSE_Message msg;
    T_ASC_PresentationContextID presID;
};

