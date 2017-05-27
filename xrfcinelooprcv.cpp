#include "xrfcinelooprcv.h"

namespace xrf {
struct StoreCallbackData
{
  CineLoopRcv* rcv;
  char* imageFileName;
  DcmFileFormat* dcmff;
  T_ASC_Association* assoc;
};

/*
 * This function.is used to indicate progress when storescp receives instance data over the
 * network. On the final call to this function (identified by progress->state == DIMSE_StoreEnd)
 * this function will store the data set which was received over the network to a file.
 * Earlier calls to this function will simply cause some information to be dumped to stdout.
 *
 * Parameters:
 *   callbackData  - [in] data for this callback function
 *   progress      - [in] The state of progress. (identifies if this is the initial or final call
 *                   to this function, or a call in between these two calls.
 *   req           - [in] The original store request message.
 *   imageFileName - [in] The path to and name of the file the information shall be written to.
 *   imageDataSet  - [in] The data set which shall be stored in the image file
 *   rsp           - [inout] the C-STORE-RSP message (will be sent after the call to this function)
 *   statusDetail  - [inout] This variable can be used to capture detailed information with regard to
 *                   the status information which is captured in the status element (0000,0900). Note
 *                   that this function does specify any such information, the pointer will be set to NULL.
 */
static void storeSCPCallback(void *callbackData,
                             T_DIMSE_StoreProgress *progress,
                             T_DIMSE_C_StoreRQ * /*req*/,
                             char * /*imageFileName*/,
                             DcmDataset **imageDataSet,
                             T_DIMSE_C_StoreRSP *rsp,
                             DcmDataset **statusDetail)
{
  // dump some information if required (depending on the progress state)
  // We can't use oflog for the pdu output, but we use a special logger for
  // generating this output. If it is set to level "INFO" we generate the
  // output, if it's set to "DEBUG" then we'll assume that there is debug output
  // generated for each PDU elsewhere.
  OFLogger progressLogger = OFLog::getLogger("dcmtk.apps." OFFIS_CONSOLE_APPLICATION ".progress");
  if (progressLogger.getChainedLogLevel() == OFLogger::INFO_LOG_LEVEL)
  {
    switch (progress->state)
    {
      case DIMSE_StoreBegin:
        COUT << "RECV: ";
        break;
      case DIMSE_StoreEnd:
        COUT << OFendl;
        break;
      default:
        COUT << '.';
        break;
    }
    COUT.flush();
  }

  // if this is the final call of this function, save the data which was received to a file
  // (note that we could also save the image somewhere else, put it in database, etc.)
  if (progress->state == DIMSE_StoreEnd)
  {
    OFString tmpStr;

    // do not send status detail information
    *statusDetail = NULL;

    // remember callback data
    StoreCallbackData *cbdata = OFstatic_cast(StoreCallbackData *, callbackData);

    // Concerning the following line: an appropriate status code is already set in the resp structure,
    // it need not be success. For example, if the caller has already detected an out of resources problem
    // then the status will reflect this.  The callback function is still called to allow cleanup.
    //rsp->DimseStatus = STATUS_Success;

    // we want to write the received information to a file only if this information
    // is present and the options opt_bitPreserving and opt_ignore are not set.
    if ((imageDataSet != NULL) && (*imageDataSet != NULL) && !cbdata->rcv->ignore())
    {
      OFString fileName;

      fileName = cbdata->imageFileName;

      // update global variables outputFileNameArray
      // (might be used in executeOnReception() and renameOnEndOfStudy)
      cbdata->rcv->outputfilenamearray().push_back(OFStandard::getFilenameFromPath(tmpStr, fileName));

      // determine the transfer syntax which shall be used to write the information to the file
      E_TransferSyntax xfer =  cbdata->rcv->writetransfersyntax();
      if (xfer == EXS_Unknown) xfer = (*imageDataSet)->getOriginalXfer();

      // store file either with meta header or as pure dataset
      OFLOG_INFO(storescpLogger, "storing DICOM file: " << fileName);
      if (OFStandard::fileExists(fileName))
      {
        OFLOG_WARN(storescpLogger, "DICOM file already exists, overwriting: " << fileName);
      }
      OFCondition cond = cbdata->dcmff->saveFile(fileName.c_str(), xfer, cbdata->rcv->sequencetype(), cbdata->rcv->grouplength(),
          cbdata->rcv->paddingtype(), OFstatic_cast(Uint32, cbdata->rcv->filepad()), OFstatic_cast(Uint32, cbdata->rcv->itempad()),
          (cbdata->rcv->usemetaheader()) ? EWM_fileformat : EWM_dataset);
      if (cond.bad())
      {
        OFLOG_ERROR(storescpLogger, "cannot write DICOM file: " << fileName << ": " << cond.text());
        rsp->DimseStatus = STATUS_STORE_Refused_OutOfResources;
      }
      else // file saved succesfully
      {
          cbdata->rcv->emitCineLoopReceivedSignal(QString(fileName.c_str()));
      }
    }
  }
}


CineLoopRcv::CineLoopRcv(const QString &outdir, const QString &fileextension, unsigned int port, long eostudy_timeout, bool promiscuous, QObject *parent)
    : QThread(parent), stopRunning(false), net(NULL), assoc(NULL), cond(EC_Normal),
      opt_outputDirectory(outdir.toStdString().c_str()), presID(0),
      opt_fileNameExtension(fileextension.toStdString().c_str()),
      opt_port(port), opt_maxPDU(ASC_DEFAULTMAXPDU), opt_useMetaheader(OFTrue),
      opt_networkTransferSyntax(EXS_Unknown), opt_writeTransferSyntax(EXS_Unknown),
      opt_groupLength(EGL_recalcGL), opt_sequenceType(EET_ExplicitLength),
      opt_paddingType(EPD_withoutPadding), opt_filepad(0),opt_itempad(0),
      opt_ignore(OFFalse), opt_promiscuous(promiscuous),opt_respondingAETitle(APPLICATIONTITLE),
      opt_blockMode(DIMSE_BLOCKING), opt_dimse_timeout(0),
      opt_endOfStudyTimeout(eostudy_timeout),opt_acse_timeout(30)
{

}

bool CineLoopRcv::init()
{
    OFString temp_str;

    WSAData winSockData;
    /* we need at least version 1.1 */
    WORD winSockVersionNeeded = MAKEWORD( 1, 1 );
    WSAStartup(winSockVersionNeeded, &winSockData);

    /* make sure data dictionary is loaded */
    if (!dcmDataDict.isDictionaryLoaded())
    {
      OFLOG_WARN(storescpLogger, "no data dictionary loaded, check environment variable: " << DCM_DICT_ENVIRONMENT_VARIABLE);
      return false;
    }

    /* if the output directory does not equal "." (default directory) */
    if (opt_outputDirectory != ".")
    {
      /* if there is a path separator at the end of the path, get rid of it */
      OFStandard::normalizeDirName(opt_outputDirectory, opt_outputDirectory);

      /* check if the specified directory exists and if it is a directory.
       * If the output directory is invalid, dump an error message and terminate execution.
       */
      if (!OFStandard::dirExists(opt_outputDirectory))
      {
        OFLOG_FATAL(storescpLogger, "specified output directory does not exist");
        return false;
      }
    }

    /* check if the output directory is writeable */
    if (!OFStandard::isWriteable(opt_outputDirectory))
    {
      OFLOG_FATAL(storescpLogger, "specified output directory is not writeable");
      return false;
    }

    /* initialize network, i.e. create an instance of T_ASC_Network*. */
    cond = ASC_initializeNetwork(NET_ACCEPTOR, OFstatic_cast(int, opt_port), opt_acse_timeout, &net);
    if (cond.bad())
    {
      OFLOG_ERROR(storescpLogger, "cannot create network: " << DimseCondition::dump(temp_str, cond));
      return false;
    }

    return true;
}

CineLoopRcv::~CineLoopRcv()
{
    /* drop the network, i.e. free memory of T_ASC_Network* structure. This call */
    /* is the counterpart of ASC_initializeNetwork(...) which was called above. */
    OFString temp_str;
    cond = ASC_dropNetwork(&net);
    if (cond.bad())
    {
        // throw exception ??
      OFLOG_ERROR(storescpLogger, DimseCondition::dump(temp_str, cond));
//      return 1;
    }

    WSACleanup();

    OFLOG_INFO(storescpLogger, "CineLoopRcv - DESTRUCTOR");
}

/*
 * This function receives DIMSE commmands over the network connection
 * and handles these commands correspondingly. Note that in case of
 * storscp only C-ECHO-RQ and C-STORE-RQ commands can be processed.
 *
 * Parameters:
 *   assoc - [in] The association (network connection to another DICOM application).
 */
OFCondition CineLoopRcv::processCommands()
{
  cond = EC_Normal;
//  T_DIMSE_Message msg;
//  T_ASC_PresentationContextID presID = 0;
  DcmDataset *statusDetail = NULL;

  // start a loop to be able to receive more than one DIMSE command
  while( cond == EC_Normal || cond == DIMSE_NODATAAVAILABLE || cond == DIMSE_OUTOFRESOURCES )
  {
    // receive a DIMSE command over the network
    cond = DIMSE_receiveCommand(assoc, DIMSE_NONBLOCKING, OFstatic_cast(int, opt_endOfStudyTimeout), &presID, &msg, &statusDetail);

    // check what kind of error occurred. If no data was
    // received, check if certain other conditions are met
    if( cond == DIMSE_NODATAAVAILABLE )
    {
      // If in addition to the fact that no data was received also option --eostudy-timeout is set and
      // if at the same time there is still a study which is considered to be open (i.e. we were actually
      // expecting to receive more objects that belong to this study) (this is the case if lastStudyInstanceUID
      // does not equal NULL), we have to consider that all objects for the current study have been received.
      // In such an "end-of-study" case, we might have to execute certain optional functions which were specified
      // by the user through command line options passed to storescp.
      if( opt_endOfStudyTimeout != -1 && !lastStudyInstanceUID.empty() )
      {
        // before we actually execute those optional functions, we need to determine the path and name
        // of the subdirectory into which the DICOM files for the last study were written.
        lastStudySubdirectoryPathAndName = subdirectoryPathAndName;

        // also, we need to clear lastStudyInstanceUID to indicate
        // that the last study is not considered to be open any more.
        lastStudyInstanceUID.clear();

        // also, we need to clear subdirectoryPathAndName
        subdirectoryPathAndName.clear();
      }
    }

    // if the command which was received has extra status
    // detail information, dump this information
    if (statusDetail != NULL)
    {
      OFLOG_WARN(storescpLogger, "Status Detail:" << OFendl << DcmObject::PrintHelper(*statusDetail));
      delete statusDetail;
    }

    // check if peer did release or abort, or if we have a valid message
    if (cond == EC_Normal)
    {
      // in case we received a valid message, process this command
      // note that storescp can only process a C-ECHO-RQ and a C-STORE-RQ
      switch (msg.CommandField)
      {
        case DIMSE_C_ECHO_RQ:
          // process C-ECHO-Request
          cond = echoSCP();
          break;
        case DIMSE_C_STORE_RQ:
          // process C-STORE-Request
          cond = storeSCP();
          break;
        default:
          // we cannot handle this kind of message
          cond = DIMSE_BADCOMMANDTYPE;
          OFLOG_ERROR(storescpLogger, "cannot handle command: 0x"
               << STD_NAMESPACE hex << OFstatic_cast(unsigned, msg.CommandField));
          break;
      }
    }
  }
  return cond;
}


OFCondition CineLoopRcv::echoSCP()
{
  OFString temp_str;
  OFLOG_INFO(storescpLogger, "Received Echo Request");
  OFLOG_DEBUG(storescpLogger, DIMSE_dumpMessage(temp_str, msg.msg.CEchoRQ, DIMSE_INCOMING, NULL, presID));

  /* the echo succeeded !! */
  cond = DIMSE_sendEchoResponse(assoc, presID, &msg.msg.CEchoRQ, STATUS_Success, NULL);
  if (cond.bad())
  {
    OFLOG_ERROR(storescpLogger, "Echo SCP Failed: " << DimseCondition::dump(temp_str, cond));
  }
  return cond;
}


DUL_PRESENTATIONCONTEXT* CineLoopRcv::findPresentationContextID(LST_HEAD * head, T_ASC_PresentationContextID presentationContextID)
{
  DUL_PRESENTATIONCONTEXT *pc;
  LST_HEAD **l;
  OFBool found = OFFalse;

  if (head == NULL)
    return NULL;

  l = &head;
  if (*l == NULL)
    return NULL;

  pc = OFstatic_cast(DUL_PRESENTATIONCONTEXT *, LST_Head(l));
  (void)LST_Position(l, OFstatic_cast(LST_NODE *, pc));

  while (pc && !found) {
    if (pc->presentationContextID == presentationContextID) {
      found = OFTrue;
    } else {
      pc = OFstatic_cast(DUL_PRESENTATIONCONTEXT *, LST_Next(l));
    }
  }
  return pc;
}


/*
 * This function processes a DIMSE C-STORE-RQ commmand that was
 * received over the network connection.
 *
 * Parameters:
 *   assoc  - [in] The association (network connection to another DICOM application).
 *   msg    - [in] The DIMSE C-STORE-RQ message that was received.
 *   presID - [in] The ID of the presentation context which was specified in the PDV which contained
 *                 the DIMSE command.
 */
OFCondition CineLoopRcv::storeSCP()
{
  T_DIMSE_C_StoreRQ *req;
  char imageFileName[2048];

  // assign the actual information of the C-STORE-RQ command to a local variable
  req = &msg.msg.CStoreRQ;


  // don't create new UID, use the study instance UID as found in object
  sprintf(imageFileName, "%s%c%s.%s%s", opt_outputDirectory.c_str(), PATH_SEPARATOR, dcmSOPClassUIDToModality(req->AffectedSOPClassUID, "UNKNOWN"),
          req->AffectedSOPInstanceUID, opt_fileNameExtension.c_str());


  // dump some information if required
  OFString str;
  OFLOG_INFO(storescpLogger, "Received Store Request: MsgID " << req->MessageID << ", ("
    << dcmSOPClassUIDToModality(req->AffectedSOPClassUID, "OT") << ")");
  OFLOG_DEBUG(storescpLogger, DIMSE_dumpMessage(str, *req, DIMSE_INCOMING, NULL, presID));

  // intialize some variables
  StoreCallbackData callbackData;
  callbackData.rcv = this;
  callbackData.assoc = assoc;
  callbackData.imageFileName = imageFileName;
  DcmFileFormat dcmff;
  callbackData.dcmff = &dcmff;

  // store SourceApplicationEntityTitle in metaheader
  if (assoc && assoc->params)
  {
    const char *aet = assoc->params->DULparams.callingAPTitle;
    if (aet) dcmff.getMetaInfo()->putAndInsertString(DCM_SourceApplicationEntityTitle, aet);
  }

  // define an address where the information which will be received over the network will be stored
  DcmDataset *dset = dcmff.getDataset();

  cond = DIMSE_storeProvider(assoc, presID, req, NULL, opt_useMetaheader, &dset,
                              storeSCPCallback, &callbackData, opt_blockMode, opt_dimse_timeout);

  // if some error occured, dump corresponding information and remove the outfile if necessary
  if (cond.bad())
  {
    OFString temp_str;
    OFLOG_ERROR(storescpLogger, "Store SCP Failed: " << DimseCondition::dump(temp_str, cond));
    // remove file
    if (!opt_ignore)
    {
      if (strcmp(imageFileName, NULL_DEVICE_NAME) != 0)
        OFStandard::deleteFile(imageFileName);
    }
  }
#ifdef _WIN32
  else if (opt_ignore)
  {
    if (strcmp(imageFileName, NULL_DEVICE_NAME) != 0)
      OFStandard::deleteFile(imageFileName); // delete the temporary file
  }
#endif

  // return return value
  return cond;
}

/** accept all presenstation contexts for unknown SOP classes,
 *  i.e. UIDs appearing in the list of abstract syntaxes
 *  where no corresponding name is defined in the UID dictionary.
 *  @param params pointer to association parameters structure
 *  @param transferSyntax transfer syntax to accept
 *  @param acceptedRole SCU/SCP role to accept
 */
OFCondition CineLoopRcv::acceptUnknownContextsWithTransferSyntax(T_ASC_Parameters * params, const char* transferSyntax, T_ASC_SC_ROLE acceptedRole)
{
  int n, i, k;
  DUL_PRESENTATIONCONTEXT *dpc;
  T_ASC_PresentationContext pc;
  OFBool accepted = OFFalse;
  OFBool abstractOK = OFFalse;

  n = ASC_countPresentationContexts(params);
  for (i = 0; i < n; i++)
  {
    cond = ASC_getPresentationContext(params, i, &pc);
    if (cond.bad()) return cond;
    abstractOK = OFFalse;
    accepted = OFFalse;

    if (dcmFindNameOfUID(pc.abstractSyntax) == NULL)
    {
      abstractOK = OFTrue;

      /* check the transfer syntax */
      for (k = 0; (k < OFstatic_cast(int, pc.transferSyntaxCount)) && !accepted; k++)
      {
        if (strcmp(pc.proposedTransferSyntaxes[k], transferSyntax) == 0)
        {
          accepted = OFTrue;
        }
      }
    }

    if (accepted)
    {
      cond = ASC_acceptPresentationContext(
        params, pc.presentationContextID,
        transferSyntax, acceptedRole);
      if (cond.bad()) return cond;
    } else {
      T_ASC_P_ResultReason reason;

      /* do not refuse if already accepted */
      dpc = findPresentationContextID(params->DULparams.acceptedPresentationContext,
                                      pc.presentationContextID);
      if ((dpc == NULL) || ((dpc != NULL) && (dpc->result != ASC_P_ACCEPTANCE)))
      {

        if (abstractOK) {
          reason = ASC_P_TRANSFERSYNTAXESNOTSUPPORTED;
        } else {
          reason = ASC_P_ABSTRACTSYNTAXNOTSUPPORTED;
        }
        /*
         * If previously this presentation context was refused
         * because of bad transfer syntax let it stay that way.
         */
        if ((dpc != NULL) && (dpc->result == ASC_P_TRANSFERSYNTAXESNOTSUPPORTED))
          reason = ASC_P_TRANSFERSYNTAXESNOTSUPPORTED;

        cond = ASC_refusePresentationContext(params, pc.presentationContextID, reason);
        if (cond.bad()) return cond;
      }
    }
  }
  return cond;
}

/** accept all presenstation contexts for unknown SOP classes,
 *  i.e. UIDs appearing in the list of abstract syntaxes
 *  where no corresponding name is defined in the UID dictionary.
 *  This method is passed a list of "preferred" transfer syntaxes.
 *  @param params pointer to association parameters structure
 *  @param transferSyntax transfer syntax to accept
 *  @param acceptedRole SCU/SCP role to accept
 */
OFCondition CineLoopRcv::acceptUnknownContextsWithPreferredTransferSyntaxes(T_ASC_Parameters * params,
                                                                           const char* transferSyntaxes[],
                                                                           int transferSyntaxCount,
                                                                           T_ASC_SC_ROLE acceptedRole)
{
  /*
  ** Accept in the order "least wanted" to "most wanted" transfer
  ** syntax.  Accepting a transfer syntax will override previously
  ** accepted transfer syntaxes.
  */
  for (int i = transferSyntaxCount - 1; i >= 0; i--)
  {
    cond =  acceptUnknownContextsWithTransferSyntax(params, transferSyntaxes[i], acceptedRole);
    if (cond.bad()) return cond;
  }
  return cond;
}

OFCondition CineLoopRcv::acceptAssociation()
{
  char buf[BUFSIZ];
  OFString temp_str;

  const char* knownAbstractSyntaxes[] =
  {
    UID_VerificationSOPClass
  };

  const char* transferSyntaxes[] = { NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL };
  int numTransferSyntaxes = 0;

  // try to receive an association. Here we either want to use blocking or
  // non-blocking, depending on if the option --eostudy-timeout is set.
    cond = ASC_receiveAssociation(net, &assoc, opt_maxPDU, NULL, NULL, OFFalse, DUL_NOBLOCK, OFstatic_cast(int, opt_endOfStudyTimeout));

  // if some kind of error occured, take care of it
  if (cond.bad())
  {
    // check what kind of error occurred. If no association was
    // received, check if certain other conditions are met
    if( cond == DUL_NOASSOCIATIONREQUEST )
    {
      // If in addition to the fact that no association was received also option --eostudy-timeout is set
      // and if at the same time there is still a study which is considered to be open (i.e. we were actually
      // expecting to receive more objects that belong to this study) (this is the case if lastStudyInstanceUID
      // does not equal NULL), we have to consider that all objects for the current study have been received.
      // In such an "end-of-study" case, we might have to execute certain optional functions which were specified
      // by the user through command line options passed to storescp.
      if( opt_endOfStudyTimeout != -1 && !lastStudyInstanceUID.empty() )
      {
        // before we actually execute those optional functions, we need to determine the path and name
        // of the subdirectory into which the DICOM files for the last study were written.
        lastStudySubdirectoryPathAndName = subdirectoryPathAndName;

        // also, we need to clear lastStudyInstanceUID to indicate
        // that the last study is not considered to be open any more.
        lastStudyInstanceUID.clear();

        // also, we need to clear subdirectoryPathAndName
        subdirectoryPathAndName.clear();

      }
    }
    // If something else was wrong we might have to dump an error message.
    else
    {
      OFLOG_ERROR(storescpLogger, "Receiving Association failed: " << DimseCondition::dump(temp_str, cond));
    }

    // no matter what kind of error occurred, we need to do a cleanup
    return cleanup();
  }

  OFLOG_INFO(storescpLogger, "Association Received");

  /* We prefer explicit transfer syntaxes.
   * If we are running on a Little Endian machine we prefer
   * LittleEndianExplicitTransferSyntax to BigEndianTransferSyntax.
   */
  if (gLocalByteOrder == EBO_LittleEndian)  /* defined in dcxfer.h */
  {
    transferSyntaxes[0] = UID_LittleEndianExplicitTransferSyntax;
    transferSyntaxes[1] = UID_BigEndianExplicitTransferSyntax;
  }
  else
  {
    transferSyntaxes[0] = UID_BigEndianExplicitTransferSyntax;
    transferSyntaxes[1] = UID_LittleEndianExplicitTransferSyntax;
  }
  transferSyntaxes[2] = UID_LittleEndianImplicitTransferSyntax;
  numTransferSyntaxes = 3;


    /* accept the Verification SOP Class if presented */
    cond = ASC_acceptContextsWithPreferredTransferSyntaxes( assoc->params, knownAbstractSyntaxes, DIM_OF(knownAbstractSyntaxes), transferSyntaxes, numTransferSyntaxes);
    if (cond.bad())
    {
      OFLOG_DEBUG(storescpLogger, DimseCondition::dump(temp_str, cond));
      return cleanup();
    }

    /* the array of Storage SOP Class UIDs comes from dcuid.h */
    cond = ASC_acceptContextsWithPreferredTransferSyntaxes( assoc->params, dcmAllStorageSOPClassUIDs, numberOfAllDcmStorageSOPClassUIDs, transferSyntaxes, numTransferSyntaxes);
    if (cond.bad())
    {
      OFLOG_DEBUG(storescpLogger, DimseCondition::dump(temp_str, cond));
      return cleanup();
    }

    if (opt_promiscuous)
    {
      /* accept everything not known not to be a storage SOP class */
      cond = acceptUnknownContextsWithPreferredTransferSyntaxes(
        assoc->params, transferSyntaxes, numTransferSyntaxes);
      if (cond.bad())
      {
        OFLOG_DEBUG(storescpLogger, DimseCondition::dump(temp_str, cond));
        return cleanup();
      }
    }


  /* set our app title */
  ASC_setAPTitles(assoc->params, NULL, NULL, opt_respondingAETitle);

  /* acknowledge or reject this association */
  cond = ASC_getApplicationContextName(assoc->params, buf);
  if ((cond.bad()) || strcmp(buf, UID_StandardApplicationContext) != 0)
  {
    /* reject: the application context name is not supported */
    T_ASC_RejectParameters rej =
    {
      ASC_RESULT_REJECTEDPERMANENT,
      ASC_SOURCE_SERVICEUSER,
      ASC_REASON_SU_APPCONTEXTNAMENOTSUPPORTED
    };

    OFLOG_INFO(storescpLogger, "Association Rejected: Bad Application Context Name: " << buf);
    cond = ASC_rejectAssociation(assoc, &rej);
    if (cond.bad())
    {
      OFLOG_DEBUG(storescpLogger, DimseCondition::dump(temp_str, cond));
    }
    return cleanup();

  }
  else
  {
    cond = ASC_acknowledgeAssociation(assoc);
    if (cond.bad())
    {
      OFLOG_ERROR(storescpLogger, DimseCondition::dump(temp_str, cond));
      return cleanup();
    }
    OFLOG_INFO(storescpLogger, "Association Acknowledged (Max Send PDV: " << assoc->sendPDVLength << ")");
    if (ASC_countAcceptedPresentationContexts(assoc->params) == 0)
      OFLOG_INFO(storescpLogger, "    (but no valid presentation contexts)");
  }

  // store previous values for later use
  lastCallingAETitle = callingAETitle;
  lastCalledAETitle = calledAETitle;
  lastCallingPresentationAddress = callingPresentationAddress;
  // store calling and called aetitle in global variables to enable
  // the --exec options using them. Enclose in quotation marks because
  // aetitles may contain space characters.
  DIC_AE callingTitle;
  DIC_AE calledTitle;
  if (ASC_getAPTitles(assoc->params, callingTitle, calledTitle, NULL).good())
  {
    callingAETitle = "\"";
    callingAETitle += OFSTRING_GUARD(callingTitle);
    callingAETitle += "\"";
    calledAETitle = "\"";
    calledAETitle += OFSTRING_GUARD(calledTitle);
    calledAETitle += "\"";
  }
  else
  {
    // should never happen
    callingAETitle.clear();
    calledAETitle.clear();
  }
  // store calling presentation address (i.e. remote hostname)
  callingPresentationAddress = OFSTRING_GUARD(assoc->params->DULparams.callingPresentationAddress);

  /* now do the real work, i.e. receive DIMSE commmands over the network connection */
  /* which was established and handle these commands correspondingly. In case of */
  /* storscp only C-ECHO-RQ and C-STORE-RQ commands can be processed. */
  cond = processCommands();

  if (cond == DUL_PEERREQUESTEDRELEASE)
  {
    OFLOG_INFO(storescpLogger, "Association Release");
    cond = ASC_acknowledgeRelease(assoc);
  }
  else if (cond == DUL_PEERABORTEDASSOCIATION)
  {
    OFLOG_INFO(storescpLogger, "Association Aborted");
  }
  else
  {
    OFLOG_ERROR(storescpLogger, "DIMSE failure (aborting association): " << DimseCondition::dump(temp_str, cond));
    /* some kind of error so abort the association */
    cond = ASC_abortAssociation(assoc);
  }

  return cleanup();
}

// instead of exiting throw exception
OFCondition& CineLoopRcv::cleanup()
{

    OFString temp_str;

    if (cond.code() == DULC_FORKEDCHILD)
        return cond;

    cond = ASC_dropSCPAssociation(assoc);
    if (cond.bad())
    {
        OFLOG_FATAL(storescpLogger, DimseCondition::dump(temp_str, cond));
        return cond;
        //exit(1);
    }
    cond = ASC_destroyAssociation(&assoc);
    if (cond.bad())
    {
        OFLOG_FATAL(storescpLogger, DimseCondition::dump(temp_str, cond));
        return cond;
        //exit(1);
    }

  return cond;
}

void CineLoopRcv::stop()
{
    QMutexLocker(&this->mutex);
    stopRunning = true;
}

void CineLoopRcv::run()
{
     while ( acceptAssociation().good() )
     {
         QMutexLocker(&this->mutex);
         if(stopRunning) break;
     }

     OFLOG_INFO(storescpLogger, "CineLoopRcv run - finished");
}

    void CineLoopRcv::emitCineLoopReceivedSignal(const QString& fullpath) {
        emit cineLoopReceived(fullpath);
    }

}
