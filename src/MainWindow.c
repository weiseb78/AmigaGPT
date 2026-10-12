#include <devices/inputevent.h>
#include <devices/printer.h>
#include <devices/prtbase.h>
#include <devices/trackdisk.h>
#include <libraries/mui.h>
#include <json-c/json.h>
#include <mui/BetterString_mcc.h>
#include <mui/Busy_mcc.h>
#include <mui/Guigfx_mcc.h>
#include <mui/NFloattext_mcc.h>
#include <mui/NList_mcc.h>
#include <mui/NListview_mcc.h>
#include <mui/ActionTextEditor_mcc.h>
#include <mui/AudioPlayer_mcc.h>
#include <mui/TextEditor_mcc.h>
#include <SDI_hook.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>
#include "AmigaGPTConfig.h"
#include "AmigaGPTTextEditor.h"
#include "streamlog.h"
#include "gui.h"
#include "menu.h"
#include "MainWindow.h"
#include "openai.h"
#include "speech.h"
#ifdef __MORPHOS__
#include <mui/Scintilla_mcc.h>
#include "ChatFindScintilla.h"
#include "ChatOutputScintilla.h"
#include "CodeBlocksViewer.h"
#endif
#include "chatmd_markers.h"
#include "utf8stream.h"

#ifndef MAIN_WINDOW_SAVED_MIN_WIDTH
#define MAIN_WINDOW_SAVED_MIN_WIDTH 320
#define MAIN_WINDOW_SAVED_MIN_HEIGHT 200
#endif

static STRPTR dupStringAlloc(CONST_STRPTR src) {
    STRPTR copy;
    ULONG n;

    if (src == NULL) {
        return NULL;
    }
    n = (ULONG)strlen(src) + 1;
    copy = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
    if (copy != NULL) {
        strncpy(copy, src, n - 1);
        copy[n - 1] = '\0';
    }
    return copy;
}

#include "SpeechProviderSettingsRequesterWindow.h"
#include <dos/dos.h>

/* Max nesting depth for B/I/U combined. Adjust as needed. */
#define MAX_STYLE_STACK 32

#define CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH (1024 * 512)
#define CHAT_OUTPUT_WIDGET_SAFE_LIMIT (60 * 1024)
#define CONVERSATION_TITLE_FALLBACK_MAX 96
/** UI refresh during stream: at most ~5 updates per second (R3). */
#define STREAM_UI_MIN_REFRESH_MS 200
/** Above this, skip live Scintilla during stream (paint once at end). */
#define STREAM_UI_LIVE_SCINTILLA_MAX 2048UL

static struct timeval streamUiLastRefresh;
static BOOL streamUiLastRefreshValid;

#ifdef __MORPHOS__
/**
 * TRUE while `sendChatMessage` is streaming assistant deltas into
 * `chatOutputTextEditorContents`. Chat Scintilla then refreshes **raw UTF-8** only
 * (no `chatOutputScintillaBuildMidiMarkdownDisplay` / link spans) — Handlungsanweisung
 * §5 / R3: kein Markdown-Parse pro Chunk. Cleared in `finishChatStream` and on early exits.
 */
static BOOL morphosChatStreamRawScintillaRefresh = FALSE;
/** TRUE only while SSE chunks arrive; FALSE for final paint after stream. */
static BOOL morphosChatLiveScintillaUpdates = FALSE;
#endif

typedef enum { STYLE_BOLD, STYLE_ITALIC, STYLE_UNDERLINE } StyleType;

/** Frees the pointer array only; each json_object must already be json_object_put. */
static void freeOpenAIResponseArray(struct json_object **responses) {
    if (responses != NULL) {
        FreeVec(responses);
    }
}

/** Release from index onward (error path); caller may have put [0..fromIndex-1]. */
static void discardOpenAIResponseArray(struct json_object **responses,
                                       UWORD fromIndex) {
    UWORD i;

    if (responses == NULL) {
        return;
    }
    for (i = fromIndex; i < RESPONSE_ARRAY_BUFFER_LENGTH && responses[i] != NULL;
         i++) {
        json_object_put(responses[i]);
    }
    FreeVec(responses);
}

/* A small stack to track active styles in the order they were opened. */
typedef struct {
    StyleType stack[MAX_STYLE_STACK];
    int top;
} StyleStack;

struct Window *mainWindow;
Object *mainWindowObject = NULL;
Object *newChatButton;
Object *deleteChatButton;
Object *sendMessageButton;
Object *attachFilesButton;
Object *clearAttachmentsButton;
Object *saveResponseFilesButton;
Object *attachmentSummaryText;
Object *chatInputTextEditor;
#ifndef __MORPHOS__
Object *chatOutputListView;
#endif
#ifdef __MORPHOS__
Object *chatOutputScroller;
#endif
Object *chatOutputTextEditor;
Object *statusBar;
Object *conversationListObject;
Object *conversationListViewObject;
Object *loadingBar;
Object *loadingBarGroup;
Object *imageInputTextEditor;
Object *createImageButton;
Object *newImageButton;
Object *deleteImageButton;
Object *imageListObject;
Object *imageView;
Object *imageViewGroup;
Object *openImageButton;
Object *saveImageCopyButton;
Object *editImageButton;
Object *attachReferenceImagesButton;
Object *clearReferenceImagesButton;
Object *referenceImageSummaryText;
Object *speechInputTextEditor;
Object *speechListObject;
Object *newSpeechButton;
Object *deleteSpeechButton;
Object *generateSpeechButton;
Object *regenerateSpeechButton;
Object *speechProfileInfo;
Object *speechWaveform;
Object *saveSpeechCopyButton;
Object *generateSpeechTextButton;
Object *attachSpeechFilesButton;
Object *clearSpeechFilesButton;
Object *speechAttachmentSummaryText;
Object *speakFileContentsCheckbox;
Object *modeRegisterGroup;
STRPTR chatOutputTextEditorContents = NULL;
static ULONG chatOutputTextEditorContentsCapacity =
    CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH;
static struct MinList pendingChatFiles;
static BOOL pendingChatFilesInitialized = FALSE;
static struct MinList pendingImageFiles;
static BOOL pendingImageFilesInitialized = FALSE;
static struct MinList pendingSpeechFiles;
static BOOL pendingSpeechFilesInitialized = FALSE;
WORD pens[NUMDRIPENS + 1];
struct Conversation *currentConversation = NULL;
/** TRUE while sendChatMessage/finishChatStream runs (MUI may pump). */
static BOOL chatSendInProgress = FALSE;
struct GeneratedImage *currentImage = NULL;
struct GeneratedSpeech {
    STRPTR title;
    STRPTR filePath;
    STRPTR text;
    STRPTR profileInfo;
};
static struct GeneratedSpeech *currentSpeech = NULL;
static STRPTR pages[4] = {NULL};
static BOOL requestInterfaceBusy = FALSE;
static BOOL haveAudioPlayer = FALSE;

struct Conversation *newConversation();
static struct Conversation *copyConversation(struct Conversation *conversation);
static struct GeneratedImage *
copyGeneratedImage(struct GeneratedImage *generatedImage);
static void sendChatMessage();

typedef enum {
    CHAT_STREAM_OK,
    CHAT_STREAM_PARTIAL,
    CHAT_STREAM_FAILED
} ChatStreamOutcome;

static ChatStreamOutcome chatStreamClassifyOutcome(UTF8 *receivedMessage);
static void finishChatStream(ChatStreamOutcome outcome, UTF8 *receivedMessage,
                             ULONG speechUtf8Index, BOOL isNewConversation,
                             struct MinList *receivedFiles, BOOL requestStream,
                             struct Conversation *streamConversation);
static BOOL conversationListContains(struct Conversation *conversation);
#ifdef __MORPHOS__
static void setConversationListInputEnabled(BOOL enabled);
static void conversationListReselectCurrent(void);
static void morphosSchedulePendingConversationSelect(void);
#endif
static void appendMessageFileSummary(struct ConversationNode *message);
static LONG loadConversations();
static LONG saveConversations();
#ifdef __MORPHOS__
#define LAST_CONVERSATION_ENV_DIR "ENV:AmigaGPT"
#define LAST_CONVERSATION_ENV_PATH "ENV:AmigaGPT/last-conversation"
#define LAST_CONVERSATION_ENVARC_DIR "ENVARC:AmigaGPT"
#define LAST_CONVERSATION_ENVARC_PATH "ENVARC:AmigaGPT/last-conversation"
#else
#define LAST_CONVERSATION_DIR "ENVARC:AmigaGPT"
#define LAST_CONVERSATION_PATH "ENVARC:AmigaGPT/last-conversation"
#endif
#define LAST_CONVERSATION_LEGACY "AMIGAGPT:last-conversation.txt"
#define LAST_CONVERSATION_NAME_MAX 512

static BOOL writeLastConversationPath(CONST_STRPTR dir, CONST_STRPTR path,
                                      CONST_STRPTR name, LONG nameLen) {
    BPTR file;
    LONG wrote;

    if (dir != NULL) {
        CreateDir(dir);
    }
    file = Open(path, MODE_NEWFILE);
    if (file == 0) {
        return FALSE;
    }
    wrote = Write(file, name, nameLen);
    Close(file);
    if (wrote != nameLen) {
        /* Partial/empty file would shadow a good ENVARC copy on restore. */
        DeleteFile(path);
        return FALSE;
    }
    return TRUE;
}

static void saveLastSelectedConversationName(struct Conversation *conversation) {
    LONG nameLen;

    if (conversation == NULL || conversation->name == NULL ||
        conversation->name[0] == '\0') {
        return;
    }
    nameLen = (LONG)strlen(conversation->name);
#ifdef __MORPHOS__
    writeLastConversationPath(LAST_CONVERSATION_ENV_DIR,
                              LAST_CONVERSATION_ENV_PATH, conversation->name,
                              nameLen);
    writeLastConversationPath(LAST_CONVERSATION_ENVARC_DIR,
                              LAST_CONVERSATION_ENVARC_PATH, conversation->name,
                              nameLen);
#else
    writeLastConversationPath(LAST_CONVERSATION_DIR, LAST_CONVERSATION_PATH,
                              conversation->name, nameLen);
#endif
}

#ifdef __MORPHOS__
static BPTR openLastConversationSource(UBYTE which) {
    switch (which) {
    case 0:
        return Open(LAST_CONVERSATION_ENV_PATH, MODE_OLDFILE);
    case 1:
        return Open(LAST_CONVERSATION_ENVARC_PATH, MODE_OLDFILE);
    case 2:
        return Open(LAST_CONVERSATION_LEGACY, MODE_OLDFILE);
    default:
        return 0;
    }
}
#endif

#ifndef __MORPHOS__
static BPTR openLastConversationFile(void) {
    BPTR file;

    file = Open(LAST_CONVERSATION_PATH, MODE_OLDFILE);
    if (file == 0) {
        file = Open(LAST_CONVERSATION_LEGACY, MODE_OLDFILE);
    }
    return file;
}
#endif

static BOOL restoreLastSelectedConversation(void) {
    BPTR file;
    STRPTR nameBuf;
    LONG fileSize;
    LONG total;
    LONG i;
    BOOL restored = FALSE;
#ifdef __MORPHOS__
    UBYTE which;
#endif

    if (conversationListObject == NULL) {
        return FALSE;
    }
#ifdef __MORPHOS__
    for (which = 0; which < 3 && !restored; which++) {
        file = openLastConversationSource(which);
        if (file == 0) {
            continue;
        }
        {
            struct FileInfoBlock fib;

            ExamineFH64(file, &fib, NULL);
            fileSize = (LONG)fib.fib_Size;
        }
        if (fileSize <= 0 || fileSize >= LAST_CONVERSATION_NAME_MAX) {
            Close(file);
            streamLogLifecycle("restore last conversation skip empty");
            continue;
        }
        nameBuf = AllocVec((ULONG)fileSize + 1, MEMF_CLEAR);
        if (nameBuf == NULL) {
            Close(file);
            return FALSE;
        }
        if (Read(file, nameBuf, fileSize) != fileSize) {
            FreeVec(nameBuf);
            Close(file);
            continue;
        }
        Close(file);
        nameBuf[fileSize] = '\0';

        get(conversationListObject, MUIA_NList_Entries, &total);
        for (i = 0; i < total; i++) {
            struct Conversation *conversation = NULL;

            DoMethod(conversationListObject, MUIM_NList_GetEntry, i,
                     &conversation);
            if (conversation != NULL && conversation->name != NULL &&
                strcmp(conversation->name, nameBuf) == 0) {
                DoMethod(conversationListObject, MUIM_NList_SetActive, i, NULL);
                streamLogLifecycle("restore last conversation ok");
                saveLastSelectedConversationName(conversation);
                restored = TRUE;
                break;
            }
        }
        FreeVec(nameBuf);
    }
    if (!restored) {
        streamLogLifecycle("restore last conversation miss");
    }
    return restored;
#else
    file = openLastConversationFile();
    if (file == 0) {
        return FALSE;
    }
#ifdef __AMIGAOS3__
    Seek(file, 0, OFFSET_END);
    fileSize = Seek(file, 0, OFFSET_BEGINNING);
#else
#ifdef __AMIGAOS4__
    fileSize = (LONG)GetFileSize(file);
#else
    {
        struct FileInfoBlock fib;

        ExamineFH64(file, &fib, NULL);
        fileSize = (LONG)fib.fib_Size;
    }
#endif
#endif
    if (fileSize <= 0 || fileSize >= LAST_CONVERSATION_NAME_MAX) {
        Close(file);
        return FALSE;
    }
    nameBuf = AllocVec((ULONG)fileSize + 1, MEMF_CLEAR);
    if (nameBuf == NULL) {
        Close(file);
        return FALSE;
    }
    if (Read(file, nameBuf, fileSize) != fileSize) {
        FreeVec(nameBuf);
        Close(file);
        return FALSE;
    }
    Close(file);
    nameBuf[fileSize] = '\0';

    get(conversationListObject, MUIA_NList_Entries, &total);
    for (i = 0; i < total; i++) {
        struct Conversation *conversation = NULL;

        DoMethod(conversationListObject, MUIM_NList_GetEntry, i, &conversation);
        if (conversation != NULL && conversation->name != NULL &&
            strcmp(conversation->name, nameBuf) == 0) {
            DoMethod(conversationListObject, MUIM_NList_SetActive, i, NULL);
            streamLogLifecycle("restore last conversation ok");
            saveLastSelectedConversationName(conversation);
            restored = TRUE;
            break;
        }
    }
    if (!restored) {
        streamLogLifecycle("restore last conversation miss");
    }
    FreeVec(nameBuf);
    return restored;
#endif
}

#ifdef __MORPHOS__
void mainWindowCaptureGeometryForConfig(void) {
    LONG left = 0;
    LONG top = 0;
    LONG width = 0;
    LONG height = 0;
    struct Screen *scr = NULL;

    if (mainWindowObject == NULL) {
        return;
    }
    get(mainWindowObject, MUIA_Window_LeftEdge, &left);
    get(mainWindowObject, MUIA_Window_TopEdge, &top);
    get(mainWindowObject, MUIA_Window_Width, &width);
    get(mainWindowObject, MUIA_Window_Height, &height);
    get(mainWindowObject, MUIA_Window_Screen, &scr);
    if (width < MAIN_WINDOW_SAVED_MIN_WIDTH ||
        height < MAIN_WINDOW_SAVED_MIN_HEIGHT) {
        return;
    }
    if (scr != NULL) {
        LONG sw = scr->Width;
        LONG sh = scr->Height;

        if (width > sw) {
            width = sw - 8;
        }
        if (height > sh) {
            height = sh - 8;
        }
        if (left < 0 || top < 0 || left > sw || top > sh) {
            left = 0;
            top = 0;
        }
        if (left + width > sw) {
            left = sw - width;
        }
        if (top + height > sh) {
            top = sh - height;
        }
        if (left < 0) {
            left = 0;
        }
        if (top < 0) {
            top = 0;
        }
    }
    configSetMainWindowLeft(left);
    configSetMainWindowTop(top);
    configSetMainWindowWidth((ULONG)width);
    configSetMainWindowHeight((ULONG)height);
}

static BOOL mainWindowSavedGeometryValid(void) {
    return mainWindowObject != NULL &&
           configGetMainWindowWidth() >= MAIN_WINDOW_SAVED_MIN_WIDTH &&
           configGetMainWindowHeight() >= MAIN_WINDOW_SAVED_MIN_HEIGHT;
}

static void mainWindowApplySavedGeometry(void) {
    LONG left;
    LONG top;
    LONG width;
    LONG height;
    struct Screen *scr = NULL;

    if (!mainWindowSavedGeometryValid()) {
        return;
    }
    left = configGetMainWindowLeft();
    top = configGetMainWindowTop();
    width = (LONG)configGetMainWindowWidth();
    height = (LONG)configGetMainWindowHeight();
    get(mainWindowObject, MUIA_Window_Screen, &scr);
    if (scr != NULL) {
        LONG sw = scr->Width;
        LONG sh = scr->Height;

        if (width > sw) {
            width = sw - 8;
        }
        if (height > sh) {
            height = sh - 8;
        }
        if (left < 0 || top < 0 || left >= sw || top >= sh) {
            left = (sw - width) / 2;
            top = (sh - height) / 2;
        }
        if (left + width > sw) {
            left = sw - width;
        }
        if (top + height > sh) {
            top = sh - height;
        }
        if (left < 0) {
            left = 0;
        }
        if (top < 0) {
            top = 0;
        }
    }
    set(mainWindowObject, MUIA_Window_Width, width);
    set(mainWindowObject, MUIA_Window_Height, height);
    set(mainWindowObject, MUIA_Window_LeftEdge, left);
    set(mainWindowObject, MUIA_Window_TopEdge, top);
}
#endif /* __MORPHOS__ */

static LONG loadImages();
static LONG saveImages();
static LONG loadSpeechHistory();
static LONG saveSpeechHistory();
static void updateSpeechControls();
static void setImageGenerationControlsDisabled(BOOL disabled);
static void generateSpeech(BOOL regenerate);
static void openImage(struct GeneratedImage *image, WORD scaledWidth,
                      WORD scaledHeight);
static void initStyleStack(StyleStack *s);
static BOOL pushStyle(StyleStack *s, StyleType style);
static BOOL popStyle(StyleStack *s, StyleType style);
static BOOL isTopStyle(const StyleStack *s, StyleType style);
static void outputStyleOn(STRPTR out, size_t outSize, StyleType style);
static void outputStyleOff(STRPTR out, size_t outSize);
static UBYTE parseMarker(CONST_STRPTR input, size_t pos, size_t len,
                         StyleType *foundStyle, const StyleStack *stack);
static BOOL ensureChatOutputBufferCapacity(ULONG required);
static void addMainWindowActions();
#ifndef __MORPHOS__
static Object *newChatOutputFloattextObject(void);
static void installChatOutputWheelHandler(void);
#endif
#ifdef __MORPHOS__
static void clearChatOutputDisplay(void);
#endif
static void updateAttachmentControls();
static void updateReferenceImageControls();
static void updateResponseFileControl();

static CONST_STRPTR jsonStringValue(struct json_object *object,
                                    CONST_STRPTR key) {
    struct json_object *value = NULL;
    if (object == NULL ||
        !json_object_object_get_ex(object, key, &value) || value == NULL ||
        json_object_is_type(value, json_type_null))
        return NULL;
    return json_object_get_string(value);
}

static ULONG chatFileCount(struct MinList *files) {
    ULONG count = 0;
    struct ChatFile *file;
    for (file = (struct ChatFile *)files->mlh_Head;
         file->node.mln_Succ != NULL;
         file = (struct ChatFile *)file->node.mln_Succ)
        count++;
    return count;
}

static BOOL chatFilePathAlreadyPresent(struct MinList *files,
                                       CONST_STRPTR path) {
    struct ChatFile *file;
    for (file = (struct ChatFile *)files->mlh_Head;
         file->node.mln_Succ != NULL;
         file = (struct ChatFile *)file->node.mln_Succ) {
        if (file->path != NULL && strcmp(file->path, path) == 0)
            return TRUE;
    }
    return FALSE;
}

static void updateAttachmentControls() {
    if (!pendingChatFilesInitialized || attachmentSummaryText == NULL)
        return;
    ULONG count = chatFileCount(&pendingChatFiles);
    UBYTE summary[128];
    if (count == 0) {
        snprintf(summary, sizeof(summary), "%s", STRING_NO_FILES_ATTACHED);
    } else {
        snprintf(summary, sizeof(summary), STRING_FILES_ATTACHED_FORMAT,
                 count);
    }
    set(attachmentSummaryText, MUIA_Text_Contents, summary);
    set(clearAttachmentsButton, MUIA_Disabled, count == 0);
}

static void updateReferenceImageControls() {
    if (!pendingImageFilesInitialized || referenceImageSummaryText == NULL)
        return;
    ULONG count = chatFileCount(&pendingImageFiles);
    UBYTE summary[128];
    if (count == 0) {
        snprintf(summary, sizeof(summary), "%s", STRING_NO_REFERENCE_IMAGES);
    } else {
        snprintf(summary, sizeof(summary),
                 STRING_REFERENCE_IMAGES_ATTACHED_FORMAT, count);
    }
    set(referenceImageSummaryText, MUIA_Text_Contents, summary);
    set(clearReferenceImagesButton, MUIA_Disabled, count == 0);
}

static STRPTR duplicateEditorText(Object *editor) {
    STRPTR source = NULL;
    STRPTR copy;
    BOOL freeSource = FALSE;

    if (editor == NULL)
        return NULL;
    if (isAROS) {
        get(editor, MUIA_String_Contents, &source);
    } else {
        source = (STRPTR)DoMethod(editor, MUIM_TextEditor_ExportText);
        freeSource = source != NULL;
    }
    if (source == NULL)
        source = (STRPTR)"";
    copy = AllocVec(strlen(source) + 1, MEMF_ANY | MEMF_CLEAR);
    if (copy != NULL)
        strcpy(copy, source);
    if (freeSource)
        FreeVec(source);
    return copy;
}

static void setEditorText(Object *editor, CONST_STRPTR text) {
    if (editor == NULL)
        return;
    if (isAROS)
        set(editor, MUIA_String_Contents, text != NULL ? text : "");
    else
        set(editor, MUIA_TextEditor_Contents, text != NULL ? text : "");
}

static void deleteDiskFile(CONST_STRPTR path) {
    if (path == NULL || strlen(path) == 0)
        return;
#if defined(__AMIGAOS3__) || defined(__MORPHOS__)
    DeleteFile(path);
#else
    Delete(path);
#endif
}

static void setActionButtonLabel(Object *button, LONG pen, CONST_STRPTR label) {
    UBYTE buffer[64];
    if (button == NULL)
        return;
    snprintf(buffer, sizeof(buffer), "\33c\33P[%ld]%s", pen, label);
    set(button, MUIA_Text_Contents, buffer);
}

#define ACTION_BTN_IDLE 0
#define ACTION_BTN_REQUEST_STOP 1
#define ACTION_BTN_SPEAK_STOP 2

static void updateActionButtonLabels(BOOL force) {
    static UBYTE lastMode = 255;
    UBYTE mode;

    if (requestInterfaceBusy)
        mode = ACTION_BTN_REQUEST_STOP;
    else if (isSpeechPlaying())
        mode = ACTION_BTN_SPEAK_STOP;
    else
        mode = ACTION_BTN_IDLE;

    if (!force && mode == lastMode)
        return;
    lastMode = mode;

    if (mode == ACTION_BTN_REQUEST_STOP) {
        setActionButtonLabel(sendMessageButton, redPen, STRING_STOP);
        setActionButtonLabel(createImageButton, redPen, STRING_STOP);
        setActionButtonLabel(generateSpeechButton, redPen, STRING_STOP);
        return;
    }
    if (mode == ACTION_BTN_SPEAK_STOP)
        setActionButtonLabel(sendMessageButton, redPen, STRING_STOP);
    else
        setActionButtonLabel(sendMessageButton, bluePen, STRING_SEND);
    setActionButtonLabel(createImageButton, bluePen, STRING_CREATE_IMAGE);
    setActionButtonLabel(generateSpeechButton, bluePen, STRING_GENERATE_SPEECH);
}

static void setActionButtonsStopMode(BOOL stopMode) {
    (void)stopMode;
    updateActionButtonLabels(TRUE);
}

static void setPrimaryActionButtonsEnabled(void) {
    if (sendMessageButton != NULL)
        set(sendMessageButton, MUIA_Disabled, FALSE);
    if (createImageButton != NULL)
        set(createImageButton, MUIA_Disabled, FALSE);
    if (generateSpeechButton != NULL)
        set(generateSpeechButton, MUIA_Disabled, FALSE);
}

static BOOL abortIfRequestBusy(void) {
    if (!requestInterfaceBusy)
        return FALSE;
    cancelActiveRequest();
    stopSpeech();
    return TRUE;
}

static void queueRequestHook(struct Hook *hook) {
    DoMethod(app, MUIM_Application_PushMethod, app, 2, MUIM_CallHook, hook);
}

static CONST_STRPTR xaiBuiltinVoiceName(CONST_STRPTR voiceId) {
    static const CONST_STRPTR pretty[] = {"Ara", "Eve", "Leo", "Rex", "Sal"};
    UBYTE i;

    if (voiceId == NULL || voiceId[0] == '\0')
        return NULL;
    for (i = 0; XAI_TTS_VOICE_NAMES[i] != NULL; i++) {
        if (strcasecmp(voiceId, XAI_TTS_VOICE_NAMES[i]) == 0)
            return pretty[i];
    }
    return NULL;
}

static BOOL copyXAIVoiceNameForId(CONST_STRPTR voiceId, STRPTR dest,
                                  ULONG destSize) {
    CONST_STRPTR pretty;
    CONST_STRPTR storedId;
    CONST_STRPTR storedName;
    STRPTR profilesStr;
    struct json_object *arr;
    int i;
    int len;

    if (dest == NULL || destSize == 0 || voiceId == NULL || voiceId[0] == '\0')
        return FALSE;
    dest[0] = '\0';
    pretty = xaiBuiltinVoiceName(voiceId);
    if (pretty == NULL)
        pretty = speechLookupXAIVoiceName(voiceId);
    if (pretty != NULL && pretty[0] != '\0' && strcmp(pretty, voiceId) != 0) {
        strncpy(dest, pretty, destSize - 1);
        dest[destSize - 1] = '\0';
        return TRUE;
    }
    pretty = xaiBuiltinVoiceName(voiceId);
    if (pretty != NULL) {
        strncpy(dest, pretty, destSize - 1);
        dest[destSize - 1] = '\0';
        return TRUE;
    }
    storedId = configGetXAITTSVoiceId();
    storedName = configGetXAITTSVoiceName();
    if (storedId != NULL && storedName != NULL && storedName[0] != '\0' &&
        strcmp(storedId, voiceId) == 0) {
        strncpy(dest, storedName, destSize - 1);
        dest[destSize - 1] = '\0';
        return TRUE;
    }
    profilesStr = configGetSpeechProfiles();
    if (profilesStr == NULL || profilesStr[0] == '\0')
        return FALSE;
    arr = json_tokener_parse(profilesStr);
    if (arr == NULL || !json_object_is_type(arr, json_type_array)) {
        if (arr != NULL)
            json_object_put(arr);
        return FALSE;
    }
    len = json_object_array_length(arr);
    for (i = 0; i < len; i++) {
        struct json_object *profile = json_object_array_get_idx(arr, i);
        struct json_object *idObj;
        struct json_object *nameObj;
        CONST_STRPTR id;
        CONST_STRPTR name;

        if (profile == NULL)
            continue;
        idObj = json_object_object_get(profile, "xaiTTSVoiceId");
        nameObj = json_object_object_get(profile, "xaiTTSVoiceName");
        id = idObj != NULL ? json_object_get_string(idObj) : NULL;
        name = nameObj != NULL ? json_object_get_string(nameObj) : NULL;
        if (id != NULL && name != NULL && name[0] != '\0' &&
            strcmp(id, voiceId) == 0) {
            strncpy(dest, name, destSize - 1);
            dest[destSize - 1] = '\0';
            json_object_put(arr);
            return TRUE;
        }
    }
    json_object_put(arr);
    return FALSE;
}

static void resolveSpeechProfileVoiceName(STRPTR dest, ULONG destSize,
                                          CONST_STRPTR profileInfo) {
    CONST_STRPTR label = STRING_MENU_OPENAI_VOICE;
    CONST_STRPTR cursor;
    UBYTE prefix[64];
    UBYTE voiceId[64];
    UBYTE voiceName[128];
    ULONG prefixLen;

    if (dest == NULL || destSize == 0)
        return;
    dest[0] = '\0';
    if (profileInfo == NULL || profileInfo[0] == '\0')
        return;
    strncpy(dest, profileInfo, destSize - 1);
    dest[destSize - 1] = '\0';
    snprintf(prefix, sizeof(prefix), "%s: ", label != NULL ? label : "Voice");
    prefixLen = strlen(prefix);
    cursor = dest;
    while (cursor[0] != '\0') {
        if (strncmp(cursor, prefix, prefixLen) == 0) {
            CONST_STRPTR start = cursor + prefixLen;
            CONST_STRPTR end = start;
            while (*end != '\0' && *end != '\n')
                end++;
            if ((ULONG)(end - start) >= sizeof(voiceId))
                break;
            memcpy(voiceId, start, (ULONG)(end - start));
            voiceId[end - start] = '\0';
            if (copyXAIVoiceNameForId(voiceId, voiceName, sizeof(voiceName)) &&
                strcmp(voiceName, voiceId) != 0) {
                UBYTE rebuilt[768];
                ULONG head = (ULONG)(cursor - dest);
                snprintf(rebuilt, sizeof(rebuilt), "%.*s%s%s%s", (int)head,
                         dest, prefix, voiceName, end);
                strncpy(dest, rebuilt, destSize - 1);
                dest[destSize - 1] = '\0';
            }
            break;
        }
        while (*cursor != '\0' && *cursor != '\n')
            cursor++;
        if (*cursor == '\n')
            cursor++;
    }
}

static void setSpeechPlayerDisplay(CONST_STRPTR filePath,
                                   CONST_STRPTR profileInfo) {
    CONST_STRPTR contents;
    UBYTE resolved[768];

    resolved[0] = '\0';
    if (profileInfo != NULL && profileInfo[0] != '\0') {
        resolveSpeechProfileVoiceName(resolved, sizeof(resolved), profileInfo);
        contents = resolved;
    } else if (currentSpeech == NULL)
        contents = (CONST_STRPTR)STRING_NO_SPEECH_SELECTED;
    else
        contents = (CONST_STRPTR)STRING_SPEECH_NO_PROFILE;
    if (speechProfileInfo != NULL)
        set(speechProfileInfo, MUIA_Text_Contents, contents);
    if (haveAudioPlayer && speechWaveform != NULL)
        set(speechWaveform, MUIA_AudioPlayer_FileName,
            filePath != NULL ? filePath : (CONST_STRPTR) "");
}

static void applySpeechWaveformPens(void) {
    if (!haveAudioPlayer || speechWaveform == NULL)
        return;
    SetAttrs(speechWaveform, MUIA_AudioPlayer_WavePen, greenPen,
             MUIA_AudioPlayer_PeakPen, yellowPen,
             MUIA_AudioPlayer_SpectrumPen, bluePen, MUIA_AudioPlayer_TextPen,
             greenPen, TAG_DONE);
}

void updatePlayButton() {
    BOOL hasSpeech;
    static BOOL lastHasSpeech = (BOOL)-1;

    updateActionButtonLabels(FALSE);

    if (!haveAudioPlayer || speechWaveform == NULL)
        return;

    DoMethod(speechWaveform, MUIM_AudioPlayer_Service);
    hasSpeech = currentSpeech != NULL && !requestInterfaceBusy;
    if (hasSpeech != lastHasSpeech) {
        set(speechWaveform, MUIA_AudioPlayer_HasSpeech,
            hasSpeech ? TRUE : FALSE);
        lastHasSpeech = hasSpeech;
    }
}

static void updateSpeechControls() {
    ULONG count = pendingSpeechFilesInitialized
                      ? chatFileCount(&pendingSpeechFiles)
                      : 0;
    UBYTE summary[128];
    STRPTR text = duplicateEditorText(speechInputTextEditor);
    BOOL hasText = text != NULL && strlen(text) > 0;
    ULONG speakFiles = FALSE;
    BOOL canGenerate;

    if (count == 0)
        strcpy(summary, STRING_NO_FILES_ATTACHED);
    else
        snprintf(summary, sizeof(summary), STRING_FILES_ATTACHED_FORMAT,
                 count);
    if (speechAttachmentSummaryText != NULL)
        set(speechAttachmentSummaryText, MUIA_Text_Contents, summary);
    if (clearSpeechFilesButton != NULL)
        set(clearSpeechFilesButton, MUIA_Disabled, count == 0);
    if (speakFileContentsCheckbox != NULL) {
        set(speakFileContentsCheckbox, MUIA_Disabled, count == 0);
        if (count == 0)
            set(speakFileContentsCheckbox, MUIA_Selected, FALSE);
        get(speakFileContentsCheckbox, MUIA_Selected, &speakFiles);
    }
    canGenerate = hasText || (count > 0 && speakFiles);
    if (!requestInterfaceBusy) {
        if (generateSpeechTextButton != NULL)
            set(generateSpeechTextButton, MUIA_Disabled, !hasText);
        if (generateSpeechButton != NULL)
            set(generateSpeechButton, MUIA_Disabled, !canGenerate);
        if (regenerateSpeechButton != NULL)
            set(regenerateSpeechButton, MUIA_Disabled,
                !canGenerate || currentSpeech == NULL);
    }
    if (saveSpeechCopyButton != NULL)
        set(saveSpeechCopyButton, MUIA_Disabled, currentSpeech == NULL);
    updatePlayButton();
    if (text != NULL)
        FreeVec(text);
}

static void setSpeechGenerationControlsDisabled(BOOL disabled) {
    ULONG count = pendingSpeechFilesInitialized
                      ? chatFileCount(&pendingSpeechFiles)
                      : 0;
    if (newSpeechButton != NULL)
        set(newSpeechButton, MUIA_Disabled, disabled);
    if (deleteSpeechButton != NULL)
        set(deleteSpeechButton, MUIA_Disabled, disabled);
    if (speechInputTextEditor != NULL)
        set(speechInputTextEditor, MUIA_Disabled, disabled);
    if (attachSpeechFilesButton != NULL)
        set(attachSpeechFilesButton, MUIA_Disabled, disabled);
    if (clearSpeechFilesButton != NULL)
        set(clearSpeechFilesButton, MUIA_Disabled, disabled || count == 0);
    if (regenerateSpeechButton != NULL)
        set(regenerateSpeechButton, MUIA_Disabled, disabled);
    if (generateSpeechTextButton != NULL)
        set(generateSpeechTextButton, MUIA_Disabled, disabled);
    if (saveSpeechCopyButton != NULL)
        set(saveSpeechCopyButton, MUIA_Disabled, disabled);
    if (speechListObject != NULL)
        set(speechListObject, MUIA_Disabled, disabled);
}

static void setChatControlsDisabled(BOOL disabled) {
    /* Match pre-upstream MorphOS fork: never MUIA_Disabled on the chat list or
     * prompt editor here. Upstream did; ObjectID + Application_Load then
     * restored a hatched/"disabled" look at startup. Chat send still gates
     * via send/new/delete buttons + chatSendInProgress; MorphOS list gating
     * stays in setConversationListInputEnabled(). */
    if (newChatButton != NULL)
        set(newChatButton, MUIA_Disabled, disabled);
    if (deleteChatButton != NULL)
        set(deleteChatButton, MUIA_Disabled, disabled);
    if (attachFilesButton != NULL)
        set(attachFilesButton, MUIA_Disabled, disabled);
    if (clearAttachmentsButton != NULL)
        set(clearAttachmentsButton, MUIA_Disabled,
            disabled || !pendingChatFilesInitialized ||
                chatFileCount(&pendingChatFiles) == 0);
}

static void setRequestInterfaceBusy(BOOL busy) {
    requestInterfaceBusy = busy;
    setChatControlsDisabled(busy);
    setImageGenerationControlsDisabled(busy);
    setSpeechGenerationControlsDisabled(busy);
    setPrimaryActionButtonsEnabled();
    setActionButtonsStopMode(busy);
    updatePlayButton();
}

static void finishActiveRequest(BOOL restoreReadyStatus) {
    BOOL cancelled = isRequestCancelled();
    endCancellableRequest();
    setRequestInterfaceBusy(FALSE);
    hideLoadingBar();
    if (cancelled)
        updateStatusBar(STRING_REQUEST_CANCELLED, yellowPen);
    else if (restoreReadyStatus)
        updateStatusBar(STRING_READY, greenPen);
    updateAttachmentControls();
    updateReferenceImageControls();
    updateSpeechControls();
    updateResponseFileControl();
}

/* Everything the user can touch while an image request is in flight. Clear is
 * a special case: it stays disabled afterwards when there is nothing to clear.
 */
static void setImageGenerationControlsDisabled(BOOL disabled) {
    set(newImageButton, MUIA_Disabled, disabled);
    set(deleteImageButton, MUIA_Disabled, disabled);
    set(imageInputTextEditor, MUIA_Disabled, disabled);
    set(attachReferenceImagesButton, MUIA_Disabled, disabled);
    set(clearReferenceImagesButton, MUIA_Disabled,
        disabled || chatFileCount(&pendingImageFiles) == 0);
}

static ULONG responseFileCount(struct Conversation *conversation) {
    ULONG count = 0;
    if (conversation == NULL)
        return 0;
    struct ConversationNode *message;
    for (message =
             (struct ConversationNode *)conversation->messages->mlh_Head;
         message->node.mln_Succ != NULL;
         message = (struct ConversationNode *)message->node.mln_Succ) {
        if (strcmp(message->role, "assistant") != 0)
            continue;
        struct ChatFile *file;
        for (file = (struct ChatFile *)message->files.mlh_Head;
             file->node.mln_Succ != NULL;
             file = (struct ChatFile *)file->node.mln_Succ) {
            if (file->fileId != NULL || file->downloadUrl != NULL ||
                file->path != NULL)
                count++;
        }
    }
    return count;
}

static void updateResponseFileControl() {
    if (saveResponseFilesButton != NULL)
        set(saveResponseFilesButton, MUIA_Disabled,
            responseFileCount(currentConversation) == 0);
}

void freeMainWindowFileState() {
    if (pendingChatFilesInitialized) {
        freeChatFiles(&pendingChatFiles);
        pendingChatFilesInitialized = FALSE;
    }
    if (pendingImageFilesInitialized) {
        freeChatFiles(&pendingImageFiles);
        pendingImageFilesInitialized = FALSE;
    }
    if (pendingSpeechFilesInitialized) {
        freeChatFiles(&pendingSpeechFiles);
        pendingSpeechFilesInitialized = FALSE;
    }
}

/** Whether the busy meter page is currently the visible one */
static BOOL loadingBarVisible = FALSE;

/**
 * Shows the loading bar and starts the busy meter animating
 **/
void showLoadingBar() {
    if (loadingBarVisible)
        return;
    loadingBarVisible = TRUE;
    set(loadingBar, MUIA_Busy_Speed, MUIV_Busy_Speed_User);
    set(loadingBarGroup, MUIA_Group_ActivePage, 1);
}

/**
 * Stops the busy meter and hides the loading bar, leaving it blank
 **/
void hideLoadingBar() {
    if (!loadingBarVisible)
        return;
    loadingBarVisible = FALSE;
    set(loadingBar, MUIA_Busy_Speed, MUIV_Busy_Speed_Off);
    set(loadingBarGroup, MUIA_Group_ActivePage, 0);
}

/**
 * Title for a new conversation: model text if non-empty, otherwise a truncated
 * first user line, otherwise the localized "New chat" string.
 **/
static STRPTR allocNewConversationTitle(struct Conversation *conv,
                                        const UTF8 *apiText) {
    if (apiText != NULL && strlen((const char *)apiText) > 0) {
        ULONG n = (ULONG)strlen((const char *)apiText) + 1;
        STRPTR out = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
        if (out != NULL) {
            strncpy(out, (const char *)apiText, n - 1);
            out[n - 1] = '\0';
        }
        return out;
    }
    if (conv != NULL && conv->messages != NULL) {
        struct MinNode *node = conv->messages->mlh_Head;
        while (node->mln_Succ != NULL) {
            struct ConversationNode *m = (struct ConversationNode *)node;
            if (strcmp((const char *)m->role, "user") == 0) {
                UTF8 *userRaw = conversationNodeGetRaw(m);
                if (userRaw != NULL && strlen((const char *)userRaw) > 0) {
                    STRPTR out = AllocVec(CONVERSATION_TITLE_FALLBACK_MAX + 1,
                                          MEMF_ANY | MEMF_CLEAR);
                    if (out == NULL)
                        return NULL;
                    ULONG pos = 0;
                    const UTF8 *s = userRaw;
                    for (; *s != '\0' && pos < CONVERSATION_TITLE_FALLBACK_MAX;
                         s++) {
                        UBYTE c = (UBYTE)*s;
                        if (c == '\n' || c == '\r')
                            out[pos++] = ' ';
                        else
                            out[pos++] = (char)c;
                    }
                    while (pos > 0 && out[pos - 1] == ' ')
                        pos--;
                    out[pos] = '\0';
                    if (pos > 0)
                        return out;
                    FreeVec(out);
                }
            }
            node = node->mln_Succ;
        }
    }
    {
        const char *d = STRING_NEW_CHAT;
        ULONG n = (ULONG)strlen(d) + 1;
        STRPTR out = AllocVec(n, MEMF_ANY | MEMF_CLEAR);
        if (out != NULL) {
            strncpy(out, d, n - 1);
            out[n - 1] = '\0';
        }
        return out;
    }
}

HOOKPROTONHNO(ConstructConversationLI_TextFunc, APTR,
              struct NList_ConstructMessage *ncm) {
    struct Conversation *entry = (struct Conversation *)ncm->entry;
    return (entry);
}
MakeHook(ConstructConversationLI_TextHook, ConstructConversationLI_TextFunc);

HOOKPROTONHNO(DestructConversationLI_TextFunc, void,
              struct NList_DestructMessage *ndm) {
    if (ndm->entry)
        freeConversation((struct Conversation *)ndm->entry);
}
MakeHook(DestructConversationLI_TextHook, DestructConversationLI_TextFunc);

HOOKPROTONHNO(DisplayConversationLI_TextFunc, void,
              struct NList_DisplayMessage *ndm) {
    static char nameBuf[512];
    struct Conversation *entry = (struct Conversation *)ndm->entry;
    if (entry == NULL) {
        ndm->strings[0] = (STRPTR) "";
    } else if (entry->name_list_display != NULL) {
        ndm->strings[0] = entry->name_list_display;
    } else if (entry->name == NULL) {
        ndm->strings[0] = (STRPTR) "";
    } else {
        STRPTR converted = CodesetsUTF8ToStr(
            CSA_DestCodeset, (Tag)systemCodeset, CSA_Source, (Tag)entry->name,
            CSA_MapForeignChars, TRUE, TAG_DONE);
        if (converted != NULL) {
            strncpy(nameBuf, converted, sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
            CodesetsFreeA(converted, NULL);
        } else {
            strncpy(nameBuf, entry->name, sizeof(nameBuf) - 1);
            nameBuf[sizeof(nameBuf) - 1] = '\0';
        }
        ndm->strings[0] = nameBuf;
    }
}
MakeHook(DisplayConversationLI_TextHook, DisplayConversationLI_TextFunc);

HOOKPROTONHNO(ConstructImageLI_TextFunc, APTR,
              struct NList_ConstructMessage *ncm) {
    struct GeneratedImage *entry = (struct GeneratedImage *)ncm->entry;
    return (entry);
}
MakeHook(ConstructImageLI_TextHook, ConstructImageLI_TextFunc);

HOOKPROTONHNO(DestructImageLI_TextFunc, void,
              struct NList_DestructMessage *ndm) {
    if (ndm->entry) {
        struct GeneratedImage *entry = (struct GeneratedImage *)ndm->entry;
        FreeVec(entry->name);
        FreeVec(entry->filePath);
        FreeVec(entry->prompt);
        FreeVec(entry);
    }
}
MakeHook(DestructImageLI_TextHook, DestructImageLI_TextFunc);

HOOKPROTONHNO(DisplayImageLI_TextFunc, void, struct NList_DisplayMessage *ndm) {
    static char imgNameBuf[512];
    struct GeneratedImage *entry = (struct GeneratedImage *)ndm->entry;
    if (entry == NULL || entry->name == NULL) {
        ndm->strings[0] = (STRPTR) "";
    } else {
        STRPTR converted = CodesetsUTF8ToStr(
            CSA_DestCodeset, (Tag)systemCodeset, CSA_Source, (Tag)entry->name,
            CSA_MapForeignChars, TRUE, TAG_DONE);
        if (converted != NULL) {
            strncpy(imgNameBuf, converted, sizeof(imgNameBuf) - 1);
            imgNameBuf[sizeof(imgNameBuf) - 1] = '\0';
            CodesetsFreeA(converted, NULL);
        } else {
            strncpy(imgNameBuf, entry->name, sizeof(imgNameBuf) - 1);
            imgNameBuf[sizeof(imgNameBuf) - 1] = '\0';
        }
        ndm->strings[0] = imgNameBuf;
    }
}
MakeHook(DisplayImageLI_TextHook, DisplayImageLI_TextFunc);

HOOKPROTONHNO(ConstructSpeechLI_TextFunc, APTR,
              struct NList_ConstructMessage *ncm) {
    return ncm->entry;
}
MakeHook(ConstructSpeechLI_TextHook, ConstructSpeechLI_TextFunc);

HOOKPROTONHNO(DestructSpeechLI_TextFunc, void,
              struct NList_DestructMessage *ndm) {
    struct GeneratedSpeech *entry =
        (struct GeneratedSpeech *)ndm->entry;
    if (entry != NULL) {
        if (entry->title != NULL)
            FreeVec(entry->title);
        if (entry->filePath != NULL)
            FreeVec(entry->filePath);
        if (entry->text != NULL)
            FreeVec(entry->text);
        if (entry->profileInfo != NULL)
            FreeVec(entry->profileInfo);
        FreeVec(entry);
    }
}
MakeHook(DestructSpeechLI_TextHook, DestructSpeechLI_TextFunc);

HOOKPROTONHNO(DisplaySpeechLI_TextFunc, void,
              struct NList_DisplayMessage *ndm) {
    struct GeneratedSpeech *entry =
        (struct GeneratedSpeech *)ndm->entry;
    ndm->strings[0] = entry != NULL && entry->title != NULL
                          ? entry->title
                          : (STRPTR)"";
}
MakeHook(DisplaySpeechLI_TextHook, DisplaySpeechLI_TextFunc);

HOOKPROTONHNONP(SpeechRowClickedFunc, void) {
    struct GeneratedSpeech *entry = NULL;
    DoMethod(speechListObject, MUIM_NList_GetEntry,
             MUIV_NList_GetEntry_Active, &entry);
    if (entry != NULL) {
        currentSpeech = entry;
        setEditorText(speechInputTextEditor, entry->text);
        stopSpeech();
        setSpeechPlayerDisplay(entry->filePath, entry->profileInfo);
    }
    updateSpeechControls();
}
MakeHook(SpeechRowClickedHook, SpeechRowClickedFunc);

#ifdef __MORPHOS__
static struct Conversation *conversationRowPending;
static BOOL morphosStartupDeferredDone;
static BOOL morphosConversationSelectEnabled;
static BOOL chatOutputRefreshPending;
static BOOL chatOutputRefreshPreserveViewport;
static BOOL chatOutputRefreshFromList;
static BOOL chatOutputMorphosListRefreshActive;

HOOKPROTONHNONP(ChatOutputRefreshDeferredFunc, void) {
    BOOL preserve = chatOutputRefreshPreserveViewport;

    chatOutputRefreshPending = FALSE;
    chatOutputMorphosListRefreshActive = chatOutputRefreshFromList;
    chatOutputRefreshFromList = FALSE;
    if (mainWindowIsShuttingDown()) {
        chatOutputMorphosListRefreshActive = FALSE;
        return;
    }
    streamLogLifecycle("displayConversation scintilla refresh begin");
    chatOutputUpdateFromBuffer(preserve);
    chatOutputMorphosListRefreshActive = FALSE;
    /* Mid-stream a deferred refresh must not clear raw/live flags -- that
     * forced the markdown+docswap path on every SSE batch and froze OS. */
    if (!chatSendInProgress) {
        morphosChatStreamRawScintillaRefresh = FALSE;
        morphosChatLiveScintillaUpdates = FALSE;
    }
    streamLogLifecycle("displayConversation scintilla refresh done");
}
MakeHook(ChatOutputRefreshDeferredHook, ChatOutputRefreshDeferredFunc);

void morphosScheduleChatOutputRefresh(BOOL preserveViewport) {
    if (mainWindowIsShuttingDown() || app == NULL) {
        return;
    }
    chatOutputRefreshPreserveViewport = preserveViewport;
    if (!chatOutputRefreshPending) {
        chatOutputRefreshFromList = FALSE;
        chatOutputRefreshPending = TRUE;
        DoMethod(app, MUIM_Application_PushMethod, app, 2, MUIM_CallHook,
                 &ChatOutputRefreshDeferredHook);
    }
}

void morphosScheduleChatOutputRefreshFromList(void) {
    if (mainWindowIsShuttingDown() || app == NULL) {
        return;
    }
    chatOutputRefreshPreserveViewport = FALSE;
    chatOutputRefreshFromList = TRUE;
    if (!chatOutputRefreshPending) {
        chatOutputRefreshPending = TRUE;
        DoMethod(app, MUIM_Application_PushMethod, app, 2, MUIM_CallHook,
                 &ChatOutputRefreshDeferredHook);
    }
}

static void morphosApplyPendingConversationSelection(void) {
    struct Conversation *conversation = conversationRowPending;

    if (mainWindowIsShuttingDown() || conversation == NULL) {
        conversationRowPending = NULL;
        return;
    }
    /* During send, keep the click queued. Messages stay pinned to the
     * stream conversation; after send we apply this selection so the UI
     * shows the chat the user picked, not the one that was streaming. */
    if (chatSendInProgress) {
        streamLogLifecycle("conversation select kept pending during chat send");
        return;
    }
    conversationRowPending = NULL;
    currentConversation = conversation;
    saveLastSelectedConversationName(currentConversation);
    streamLogLifecycle("conversation select deferred begin");
    codeBlocksViewerDismiss();
    displayConversation(currentConversation);
    refreshViewCodeBlocksMenuState();
    streamLogLifecycle("conversation select deferred done");
    if (chatInputTextEditor != NULL) {
        DoMethod(chatInputTextEditor, MUIM_GoActive);
    }
}

static void morphosFlushPendingPushMethods(void) {
    if (app == NULL) {
        return;
    }
    if (mainWindowIsShuttingDown()) {
        conversationRowPending = NULL;
        chatOutputRefreshPending = FALSE;
        chatOutputRefreshFromList = FALSE;
        chatOutputScintillaCancelDeferredStyles();
        chatOutputScintillaCancelPendingCodeblockOpen();
        streamLogLifecycle("morphos flush pushmethods skipped shutdown");
        return;
    }
    ULONG muiSig = 0;
    ULONG pass;

    streamLogLifecycle("morphos flush pushmethods begin");
    for (pass = 0; pass < 12; pass++) {
        BOOL hadPending = chatOutputRefreshPending ||
                          (!chatSendInProgress &&
                           conversationRowPending != NULL) ||
                          chatOutputScintillaHasDeferredWorkPending();

        (void)DoMethod(app, MUIM_Application_CheckRefresh);
        (void)DoMethod(app, MUIM_Application_NewInput, &muiSig);
        if (!hadPending) {
            break;
        }
    }
    streamLogLifecycle("morphos flush pushmethods done");
}

static void morphosEnableConversationSelect(void);

void morphosRunStartupDeferred(void) {
    if (morphosStartupDeferredDone || mainWindowObject == NULL ||
        chatOutputTextEditor == NULL) {
        return;
    }
    morphosStartupDeferredDone = TRUE;
    streamLogLifecycle("morphos startup deferred begin");
    chatOutputScintillaFinishViewerInit(chatOutputTextEditor);
    streamLogBootPhase("chat scintilla init");
    streamLogLifecycle("morphos startup deferred finish done");
    chatOutputScintillaInstallMouseUpGuard();
    streamLogLifecycle("morphos startup deferred mouse guard done");
    mainWindowApplySavedGeometry();
    morphosEnableConversationSelect();
}

HOOKPROTONHNONP(ConversationRowClickedDeferredFunc, void) {
    if (!morphosConversationSelectEnabled || mainWindowIsShuttingDown()) {
        return;
    }
    morphosApplyPendingConversationSelection();
}
MakeHook(ConversationRowClickedDeferredHook, ConversationRowClickedDeferredFunc);

static void morphosSchedulePendingConversationSelect(void) {
    if (conversationRowPending == NULL || app == NULL ||
        mainWindowIsShuttingDown() || !morphosConversationSelectEnabled) {
        return;
    }
    streamLogLifecycle("conversation select schedule after chat send");
    DoMethod(app, MUIM_Application_PushMethod, app, 2, MUIM_CallHook,
             &ConversationRowClickedDeferredHook);
}

static void morphosEnableConversationSelect(void) {
    struct Conversation *active = NULL;
    struct Conversation *pick = NULL;

    morphosConversationSelectEnabled = TRUE;
    streamLogLifecycle("morphos conversation select enabled");
    if (conversationListObject != NULL) {
        DoMethod(conversationListObject, MUIM_NList_GetEntry,
                 MUIV_NList_GetEntry_Active, &active);
        if (active == NULL) {
            DoMethod(conversationListObject, MUIM_NList_GetEntry, 0, &active);
            if (active != NULL) {
                DoMethod(conversationListObject, MUIM_NList_SetActive, 0,
                         NULL);
                streamLogLifecycle("morphos conversation auto-select first");
            }
        } else {
            streamLogLifecycle("morphos conversation auto-select active");
        }
    }
    if (conversationRowPending != NULL) {
        pick = conversationRowPending;
    } else if (active != NULL) {
        pick = active;
    }
    if (pick != NULL && app != NULL) {
        conversationRowPending = pick;
        DoMethod(app, MUIM_Application_PushMethod, app, 2, MUIM_CallHook,
                 &ConversationRowClickedDeferredHook);
    } else {
        streamLogLifecycle("morphos conversation auto-select none");
    }
}

HOOKPROTONHNONP(ConversationRowClickedFunc, void) {
    struct Conversation *conversation;

    DoMethod(conversationListObject, MUIM_NList_GetEntry,
             MUIV_NList_GetEntry_Active, &conversation);
    if (conversation == NULL || app == NULL) {
        return;
    }
    if (chatSendInProgress) {
        conversationRowPending = conversation;
        streamLogLifecycle("conversation select queued during chat send");
        return;
    }
    conversationRowPending = conversation;
    if (!morphosConversationSelectEnabled) {
        streamLogLifecycle("conversation select queued until startup ready");
        return;
    }
    DoMethod(app, MUIM_Application_PushMethod, app, 2, MUIM_CallHook,
             &ConversationRowClickedDeferredHook);
}

#else /* !__MORPHOS__ */
HOOKPROTONHNONP(ConversationRowClickedFunc, void) {
    set(chatInputTextEditor, MUIA_TextEditor_FixedFont, TRUE);
    struct Conversation *conversation;
    DoMethod(conversationListObject, MUIM_NList_GetEntry,
             MUIV_NList_GetEntry_Active, &conversation);
    if (conversation) {
        currentConversation = conversation;
        saveLastSelectedConversationName(currentConversation);
        displayConversation(currentConversation);
        updateResponseFileControl();
        DoMethod(chatInputTextEditor, MUIM_GoActive);
    }
}
#endif
MakeHook(ConversationRowClickedHook, ConversationRowClickedFunc);

HOOKPROTONHNONP(ImageRowClickedFunc, void) {
    set(imageInputTextEditor, MUIA_Disabled, FALSE);

    struct GeneratedImage *image = NULL;
    DoMethod(imageListObject, MUIM_NList_GetEntry, MUIV_NList_GetEntry_Active,
             &image);
    if (image) {
        set(createImageButton, MUIA_Disabled, TRUE);
        set(openImageButton, MUIA_Disabled, FALSE);
        set(saveImageCopyButton, MUIA_Disabled, FALSE);
        set(editImageButton, MUIA_Disabled, FALSE);
        if (isAROS) {
            set(imageInputTextEditor, MUIA_String_Contents, image->prompt);
        } else {
            set(imageInputTextEditor, MUIA_TextEditor_Contents, image->prompt);
        }
        currentImage = image;
        DoMethod(imageViewGroup, MUIM_Group_InitChange);
        DoMethod(imageViewGroup, OM_REMMEMBER, imageView);
        MUI_DisposeObject(imageView);
        // clang-format off
        imageView = GuigfxObject,
            MUIA_Guigfx_FileName, image->filePath,
            MUIA_Guigfx_Quality, MUIV_Guigfx_Quality_Low,
            MUIA_Guigfx_ScaleMode, NISMF_SCALEFREE | NISMF_KEEPASPECT_PICTURE,
            MUIA_Guigfx_Transparency, NITRF_MASK,
        End;
        // clang-format on
        DoMethod(imageViewGroup, OM_ADDMEMBER, imageView);
        DoMethod(imageViewGroup, MUIM_Group_MoveMember, imageView, 0);
        DoMethod(imageViewGroup, MUIM_Group_ExitChange);
    }
}
MakeHook(ImageRowClickedHook, ImageRowClickedFunc);

HOOKPROTONHNONP(NewChatButtonClickedFunc, void) {
    currentConversation = NULL;
#ifdef __MORPHOS__
    clearChatOutputDisplay();
#else
    DoMethod(chatOutputTextEditor, MUIM_NList_Clear);
#endif
#ifdef __MORPHOS__
    codeBlocksViewerDismiss();
    refreshViewCodeBlocksMenuState();
#endif
    updateResponseFileControl();
    DoMethod(chatInputTextEditor, MUIM_GoActive);
}
MakeHook(NewChatButtonClickedHook, NewChatButtonClickedFunc);

HOOKPROTONHNONP(DeleteChatButtonClickedFunc, void) {
    DoMethod(conversationListObject, MUIM_NList_Remove,
             MUIV_NList_Remove_Active);
    currentConversation = NULL;
#ifdef __MORPHOS__
    clearChatOutputDisplay();
#else
    DoMethod(chatOutputTextEditor, MUIM_NList_Clear);
#endif
#ifdef __MORPHOS__
    codeBlocksViewerDismiss();
    refreshViewCodeBlocksMenuState();
#endif
    updateResponseFileControl();
    saveConversations();
}
MakeHook(DeleteChatButtonClickedHook, DeleteChatButtonClickedFunc);

static void addPendingFile(struct MinList *files, CONST_STRPTR path,
                           CONST_STRPTR name, BOOL imagesOnly) {
    if (path == NULL || name == NULL || chatFilePathAlreadyPresent(files, path))
        return;
    UTF8 *nameUTF8 = CodesetsUTF8Create(
        CSA_SourceCodeset, (Tag)systemCodeset, CSA_Source, (Tag)name,
        CSA_MapForeignChars, TRUE, TAG_DONE);
    /* Work out the type now rather than when the message is sent: reading the
     * magic database takes a moment, and here it is part of an action the user
     * just asked for. NULL simply means no database is installed, in which case
     * the type is guessed from the file name at send time instead. */
    STRPTR detectedMime = detectMimeTypeFromContents(path);
    /* An image request can only carry pictures, and what the provider says
     * about anything else is not something a user could act on, so the file is
     * turned away here instead. */
    if (imagesOnly && !attachmentLooksLikeImage(path, detectedMime)) {
        displayError(STRING_ERROR_NOT_AN_IMAGE);
    } else if (!addChatFile(files, path,
                            nameUTF8 != NULL ? nameUTF8 : (UTF8 *)name,
                            detectedMime, NULL, NULL, NULL)) {
        displayError(STRING_ERROR_MEMORY_CONVERSATION_NODE);
    }
    if (detectedMime != NULL)
        FreeVec(detectedMime); /* addChatFile took its own copy */
    if (nameUTF8 != NULL)
        CodesetsFreeA(nameUTF8, NULL);
}

static void requestFilesIntoList(struct MinList *files, CONST_STRPTR title,
                                 BOOL imagesOnly) {
    struct FileRequester *fileReq =
        AllocAslRequestTags(ASL_FileRequest, TAG_END);
    if (fileReq == NULL)
        return;
    if (AslRequestTags(fileReq, ASLFR_Window, mainWindow, ASLFR_TitleText,
                       title, ASLFR_DoMultiSelect, TRUE, ASLFR_RejectIcons,
                       TRUE, TAG_DONE)) {
        if (fileReq->fr_NumArgs > 0 && fileReq->fr_ArgList != NULL) {
            for (LONG i = 0; i < fileReq->fr_NumArgs; i++) {
                UBYTE fullPath[1024] = {0};
                struct WBArg *argument = &fileReq->fr_ArgList[i];
                if (NameFromLock(argument->wa_Lock, fullPath,
                                 sizeof(fullPath))) {
                    AddPart(fullPath, argument->wa_Name, sizeof(fullPath));
                    addPendingFile(files, fullPath, argument->wa_Name,
                                   imagesOnly);
                }
            }
        } else if (fileReq->fr_File != NULL &&
                   strlen(fileReq->fr_File) > 0) {
            ULONG fullPathLength = strlen(fileReq->fr_Drawer) +
                                   strlen(fileReq->fr_File) + 2;
            STRPTR fullPath =
                AllocVec(fullPathLength, MEMF_ANY | MEMF_CLEAR);
            if (fullPath != NULL) {
                strncpy(fullPath, fileReq->fr_Drawer,
                        strlen(fileReq->fr_Drawer));
                AddPart(fullPath, fileReq->fr_File, fullPathLength);
                addPendingFile(files, fullPath, fileReq->fr_File, imagesOnly);
                FreeVec(fullPath);
            }
        }
    }
    FreeAslRequest(fileReq);
}

HOOKPROTONHNONP(AttachFilesButtonClickedFunc, void) {
    requestFilesIntoList(&pendingChatFiles, STRING_ATTACH_FILES, FALSE);
    updateAttachmentControls();
}
MakeHook(AttachFilesButtonClickedHook, AttachFilesButtonClickedFunc);

HOOKPROTONHNONP(ClearAttachmentsButtonClickedFunc, void) {
    freeChatFiles(&pendingChatFiles);
    updateAttachmentControls();
}
MakeHook(ClearAttachmentsButtonClickedHook,
         ClearAttachmentsButtonClickedFunc);

HOOKPROTONHNONP(AttachReferenceImagesButtonClickedFunc, void) {
    requestFilesIntoList(&pendingImageFiles, STRING_ATTACH_REFERENCE_IMAGES,
                         TRUE);
    updateReferenceImageControls();
}
MakeHook(AttachReferenceImagesButtonClickedHook,
         AttachReferenceImagesButtonClickedFunc);

HOOKPROTONHNONP(ClearReferenceImagesButtonClickedFunc, void) {
    freeChatFiles(&pendingImageFiles);
    updateReferenceImageControls();
}
MakeHook(ClearReferenceImagesButtonClickedHook,
         ClearReferenceImagesButtonClickedFunc);

static STRPTR readSpeechFile(CONST_STRPTR path) {
    BPTR file;
    LONG length;
    STRPTR contents;
    LONG i;

    if (path == NULL)
        return NULL;
    file = Open(path, MODE_OLDFILE);
    if (file == 0)
        return NULL;
#ifdef __AMIGAOS3__
    Seek(file, 0, OFFSET_END);
    length = Seek(file, 0, OFFSET_BEGINNING);
#elif defined(__AMIGAOS4__)
    length = (LONG)GetFileSize(file);
#else
    {
        struct FileInfoBlock fib;
        ExamineFH64(file, &fib, NULL);
        length = (LONG)fib.fib_Size;
    }
#endif
    if (length <= 0 || length > 8 * 1024 * 1024) {
        Close(file);
        return NULL;
    }
    contents = AllocVec(length + 1, MEMF_ANY | MEMF_CLEAR);
    if (contents == NULL || Read(file, contents, length) != length) {
        if (contents != NULL)
            FreeVec(contents);
        Close(file);
        return NULL;
    }
    Close(file);
    for (i = 0; i < length; i++) {
        if (contents[i] == '\0')
            contents[i] = ' ';
    }
    return contents;
}

static STRPTR buildSpeechText(void) {
    STRPTR result = duplicateEditorText(speechInputTextEditor);
    ULONG selected = FALSE;
    struct ChatFile *file;

    if (result == NULL)
        return NULL;
    get(speakFileContentsCheckbox, MUIA_Selected, &selected);
    if (!selected)
        return result;

    for (file = (struct ChatFile *)pendingSpeechFiles.mlh_Head;
         file->node.mln_Succ != NULL;
         file = (struct ChatFile *)file->node.mln_Succ) {
        STRPTR contents = readSpeechFile(file->path);
        STRPTR expanded;
        ULONG oldLength;
        ULONG addLength;
        if (contents == NULL)
            continue;
        oldLength = strlen(result);
        addLength = strlen(contents);
        expanded = AllocVec(oldLength + addLength + 3,
                            MEMF_ANY | MEMF_CLEAR);
        if (expanded != NULL) {
            strcpy(expanded, result);
            if (oldLength > 0)
                strcat(expanded, "\n\n");
            strcat(expanded, contents);
            FreeVec(result);
            result = expanded;
        }
        FreeVec(contents);
    }
    return result;
}

static STRPTR copySpeechString(CONST_STRPTR text) {
    STRPTR copy;
    ULONG length;

    if (text == NULL)
        return NULL;
    length = strlen(text);
    copy = AllocVec(length + 1, MEMF_ANY | MEMF_CLEAR);
    if (copy != NULL)
        memcpy(copy, text, length);
    return copy;
}

static void appendProfileLine(STRPTR buffer, ULONG size, CONST_STRPTR label,
                              CONST_STRPTR value) {
    ULONG used;

    if (buffer == NULL || label == NULL || value == NULL || value[0] == '\0')
        return;
    used = strlen(buffer);
    if (used + 4 >= size)
        return;
    if (used > 0)
        buffer[used++] = '\n';
    snprintf(buffer + used, size - used, "%s: %s", label, value);
}

static STRPTR formatSpeechProfileInfo(
    const struct SpeechRequestSettings *settings) {
    UBYTE buffer[768];
    CONST_STRPTR voice = NULL;
    CONST_STRPTR model = NULL;
    UBYTE extra[64];

    if (settings == NULL)
        return NULL;
    memset(buffer, 0, sizeof(buffer));
    appendProfileLine(buffer, sizeof(buffer), STRING_SPEECH_PROFILE,
                      settings->activeProfileName);
    if (settings->speechSystem <= SPEECH_SYSTEM_OPENVOX)
        appendProfileLine(buffer, sizeof(buffer), STRING_SPEECH_SYSTEM_LABEL,
                          SPEECH_SYSTEM_NAMES[settings->speechSystem]);

    extra[0] = '\0';
    switch (settings->speechSystem) {
    case SPEECH_SYSTEM_34:
    case SPEECH_SYSTEM_37:
        snprintf(extra, sizeof(extra), "%s, %s",
                 settings->narratorSex ? STRING_NARRATOR_SEX_FEMALE
                                       : STRING_NARRATOR_SEX_MALE,
                 settings->narratorMode ? STRING_NARRATOR_MODE_ROBOTIC
                                        : STRING_NARRATOR_MODE_NATURAL);
        appendProfileLine(buffer, sizeof(buffer), STRING_MENU_OPENAI_VOICE,
                          extra);
        snprintf(extra, sizeof(extra), "%u", settings->narratorRate);
        appendProfileLine(buffer, sizeof(buffer), STRING_NARRATOR_RATE_WPM,
                          extra);
        snprintf(extra, sizeof(extra), "%u", settings->narratorPitch);
        appendProfileLine(buffer, sizeof(buffer), STRING_NARRATOR_PITCH_HZ,
                          extra);
        appendProfileLine(buffer, sizeof(buffer), STRING_MENU_SPEECH_ACCENT,
                          settings->accentPath);
        break;
    case SPEECH_SYSTEM_FLITE:
        if (settings->fliteVoice <= SPEECH_FLITE_VOICE_SLT)
            voice = SPEECH_FLITE_VOICE_NAMES[settings->fliteVoice];
        appendProfileLine(buffer, sizeof(buffer), STRING_MENU_OPENAI_VOICE,
                          voice);
        break;
    case SPEECH_SYSTEM_OPENAI:
        if (settings->openAiTtsVoice <= OPENAI_TTS_VOICE_VERSE)
            voice = OPENAI_TTS_VOICE_NAMES[settings->openAiTtsVoice];
        model = settings->openAiTtsModelId;
        appendProfileLine(buffer, sizeof(buffer), STRING_MENU_OPENAI_VOICE,
                          voice);
        appendProfileLine(buffer, sizeof(buffer),
                          STRING_MENU_SPEECH_OPENAI_MODEL, model);
        break;
    case SPEECH_SYSTEM_ELEVENLABS:
        voice = settings->elevenLabsVoiceName;
        if (voice == NULL || voice[0] == '\0')
            voice = settings->elevenLabsVoiceID;
        model = settings->elevenLabsModelName;
        if (model == NULL || model[0] == '\0')
            model = settings->elevenLabsModel;
        appendProfileLine(buffer, sizeof(buffer), STRING_MENU_OPENAI_VOICE,
                          voice);
        appendProfileLine(buffer, sizeof(buffer),
                          STRING_MENU_SPEECH_OPENAI_MODEL, model);
        break;
    case SPEECH_SYSTEM_XAI: {
        UBYTE xaiName[128];
        CONST_STRPTR voiceId = settings->xaiVoiceId;
        if (voiceId == NULL || voiceId[0] == '\0') {
            if (settings->xaiVoice >= 0 &&
                XAI_TTS_VOICE_NAMES[settings->xaiVoice] != NULL)
                voiceId = XAI_TTS_VOICE_NAMES[settings->xaiVoice];
        }
        voice = settings->xaiVoiceName;
        if (voice == NULL || voice[0] == '\0') {
            if (copyXAIVoiceNameForId(voiceId, xaiName, sizeof(xaiName)))
                voice = xaiName;
            else
                voice = voiceId;
        }
        appendProfileLine(buffer, sizeof(buffer), STRING_MENU_OPENAI_VOICE,
                          voice);
        if (voice != NULL && voiceId != NULL && voice[0] != '\0' &&
            strcmp(voice, voiceId) != 0)
            configSetXAITTSVoiceName(voice);
    }
        appendProfileLine(buffer, sizeof(buffer), STRING_SPEECH_LANGUAGE,
                          settings->xaiLanguage);
        break;
    case SPEECH_SYSTEM_OPENVOX:
        appendProfileLine(buffer, sizeof(buffer), STRING_MENU_OPENAI_VOICE,
                          settings->openVoxVoice);
        appendProfileLine(buffer, sizeof(buffer),
                          STRING_MENU_SPEECH_OPENAI_MODEL,
                          settings->openVoxModel);
        appendProfileLine(buffer, sizeof(buffer), STRING_SPEECH_LANGUAGE,
                          settings->openVoxLanguage);
        break;
    }
    if (buffer[0] == '\0')
        return NULL;
    return copySpeechString(buffer);
}

static STRPTR speechTitle(CONST_STRPTR text) {
    ULONG length = text != NULL ? strlen(text) : 0;
    STRPTR title;
    if (length > 32)
        length = 32;
    title = AllocVec(length + 1, MEMF_ANY | MEMF_CLEAR);
    if (title != NULL && length > 0)
        memcpy(title, text, length);
    return title;
}

static void generateSpeech(BOOL regenerate) {
    STRPTR text;
    UBYTE path[64];
    UBYTE id[12];
    CONST_STRPTR idChars = "abcdefghijklmnopqrstuvwxyz0123456789";
    struct GeneratedSpeech *entry;
    AudioFormat format = AUDIO_FORMAT_WAV;
    BPTR speechDir;
    UBYTE i;
    struct SpeechRequestSettings settings;
    BOOL generated;
    BOOL createdNewEntry = FALSE;
    STRPTR profileInfo = NULL;

    memset(id, 0, sizeof(id));
    memset(path, 0, sizeof(path));
    entry = regenerate ? currentSpeech : NULL;
    text = buildSpeechText();
    if (text == NULL || strlen(text) == 0) {
        if (text != NULL)
            FreeVec(text);
        return;
    }
    speechDir = CreateDir("AMIGAGPT:speech");
    if (speechDir != 0)
        UnLock(speechDir);
    if (entry != NULL && entry->filePath != NULL) {
        strncpy(path, entry->filePath, sizeof(path) - 1);
        path[sizeof(path) - 1] = '\0';
    } else {
        srand(time(NULL));
        for (i = 0; i < 10; i++)
            id[i] = idChars[rand() % strlen(idChars)];
        snprintf(path, sizeof(path), "AMIGAGPT:speech/%s.wav", id);
    }

    beginCancellableRequest();
    setRequestInterfaceBusy(TRUE);
    updateStatusBar(STRING_GENERATING_SPEECH, yellowPen);
    showLoadingBar();
    configGetSpeechRequestSettings(&settings);
    generated = speakTextWithSettings(text, path, &format, &settings);
    if (generated && !isRequestCancelled())
        profileInfo = formatSpeechProfileInfo(&settings);
    configFreeSpeechRequestSettings(&settings);
    if (!generated || isRequestCancelled()) {
        if (entry == NULL)
            deleteDiskFile(path);
        FreeVec(text);
        finishActiveRequest(generated && !isRequestCancelled());
        return;
    }

    if (entry == NULL) {
        entry = AllocVec(sizeof(*entry), MEMF_ANY | MEMF_CLEAR);
        if (entry == NULL) {
            deleteDiskFile(path);
            FreeVec(text);
            if (profileInfo != NULL)
                FreeVec(profileInfo);
            finishActiveRequest(FALSE);
            displayError(STRING_ERROR_MEMORY_CONVERSATION_NODE);
            return;
        }
        entry->filePath = AllocVec(strlen(path) + 1, MEMF_ANY | MEMF_CLEAR);
        if (entry->filePath != NULL)
            strcpy(entry->filePath, path);
        else {
            deleteDiskFile(path);
            FreeVec(entry);
            FreeVec(text);
            if (profileInfo != NULL)
                FreeVec(profileInfo);
            finishActiveRequest(FALSE);
            displayError(STRING_ERROR_MEMORY_CONVERSATION_NODE);
            return;
        }
        DoMethod(speechListObject, MUIM_NList_InsertSingle, entry,
                 MUIV_NList_Insert_Top);
        DoMethod(speechListObject, MUIM_NList_SetActive, 0, NULL);
        createdNewEntry = TRUE;
    } else {
        if (entry->text != NULL)
            FreeVec(entry->text);
        if (entry->title != NULL)
            FreeVec(entry->title);
        if (entry->profileInfo != NULL)
            FreeVec(entry->profileInfo);
    }
    entry->text = text;
    entry->title = speechTitle(text);
    entry->profileInfo = profileInfo;
    currentSpeech = entry;
    DoMethod(speechListObject, MUIM_NList_Redraw, MUIV_NList_Redraw_Active);
    if (saveSpeechHistory() != RETURN_OK && createdNewEntry)
        deleteDiskFile(path);
    setSpeechPlayerDisplay(path, entry->profileInfo);
    finishActiveRequest(TRUE);
}

HOOKPROTONHNONP(NewSpeechButtonClickedFunc, void) {
    currentSpeech = NULL;
    setEditorText(speechInputTextEditor, "");
    freeChatFiles(&pendingSpeechFiles);
    setSpeechPlayerDisplay(NULL, NULL);
    updateSpeechControls();
    DoMethod(speechInputTextEditor, MUIM_GoActive);
}
MakeHook(NewSpeechButtonClickedHook, NewSpeechButtonClickedFunc);

HOOKPROTONHNONP(DeleteSpeechButtonClickedFunc, void) {
    struct GeneratedSpeech *entry = NULL;
    DoMethod(speechListObject, MUIM_NList_GetEntry,
             MUIV_NList_GetEntry_Active, &entry);
    if (entry != NULL && entry->filePath != NULL)
        deleteDiskFile(entry->filePath);
    DoMethod(speechListObject, MUIM_NList_Remove, MUIV_NList_Remove_Active);
    currentSpeech = NULL;
    setEditorText(speechInputTextEditor, "");
    stopSpeech();
    setSpeechPlayerDisplay(NULL, NULL);
    saveSpeechHistory();
    updateSpeechControls();
}
MakeHook(DeleteSpeechButtonClickedHook, DeleteSpeechButtonClickedFunc);

HOOKPROTONHNONP(GenerateSpeechDeferredFunc, void) {
    if (requestInterfaceBusy)
        return;
    generateSpeech(FALSE);
}
MakeHook(GenerateSpeechDeferredHook, GenerateSpeechDeferredFunc);

HOOKPROTONHNONP(GenerateSpeechButtonClickedFunc, void) {
    if (abortIfRequestBusy())
        return;
    queueRequestHook(&GenerateSpeechDeferredHook);
}
MakeHook(GenerateSpeechButtonClickedHook, GenerateSpeechButtonClickedFunc);

HOOKPROTONHNONP(RegenerateSpeechButtonClickedFunc, void) {
    generateSpeech(TRUE);
}
MakeHook(RegenerateSpeechButtonClickedHook,
         RegenerateSpeechButtonClickedFunc);

HOOKPROTONHNONP(AttachSpeechFilesButtonClickedFunc, void) {
    ULONG oldCount = chatFileCount(&pendingSpeechFiles);
    requestFilesIntoList(&pendingSpeechFiles, STRING_ATTACH_FILES, FALSE);
    if (oldCount == 0 && chatFileCount(&pendingSpeechFiles) > 0)
        set(speakFileContentsCheckbox, MUIA_Selected, TRUE);
    updateSpeechControls();
}
MakeHook(AttachSpeechFilesButtonClickedHook,
         AttachSpeechFilesButtonClickedFunc);

HOOKPROTONHNONP(ClearSpeechFilesButtonClickedFunc, void) {
    freeChatFiles(&pendingSpeechFiles);
    updateSpeechControls();
}
MakeHook(ClearSpeechFilesButtonClickedHook,
         ClearSpeechFilesButtonClickedFunc);

HOOKPROTONHNONP(SpeechTextChangedFunc, void) {
    if (!isAROS && speechInputTextEditor != NULL)
        set(speechInputTextEditor, MUIA_TextEditor_HasChanged, FALSE);
    updateSpeechControls();
}
MakeHook(SpeechTextChangedHook, SpeechTextChangedFunc);

HOOKPROTONHNONP(SaveSpeechCopyButtonClickedFunc, void) {
    STRPTR filePath;
    STRPTR fileExtension;
    UBYTE fileName[16];
    struct FileRequester *fileReq;

    if (currentSpeech == NULL || currentSpeech->filePath == NULL)
        return;
    filePath = currentSpeech->filePath;
    fileExtension = strrchr(filePath, '.');
    if (fileExtension == NULL)
        fileExtension = ".wav";
    snprintf(fileName, sizeof(fileName), "speech%s", fileExtension);
    fileReq = AllocAslRequestTags(ASL_FileRequest, TAG_END);
    if (fileReq != NULL) {
        if (AslRequestTags(fileReq, ASLFR_Window, mainWindow, ASLFR_TitleText,
                           STRING_SAVE_SPEECH_COPY, ASLFR_InitialFile, fileName,
                           ASLFR_InitialDrawer, "SYS:", ASLFR_DoSaveMode, TRUE,
                           TAG_DONE)) {
            STRPTR savePath = fileReq->fr_Drawer;
            STRPTR saveName = fileReq->fr_File;
            UWORD fullPathLength = strlen(savePath) + strlen(saveName) + 2;
            STRPTR fullPath = AllocVec(fullPathLength, MEMF_CLEAR);
            if (fullPath != NULL) {
                strncpy(fullPath, savePath, strlen(savePath));
                AddPart(fullPath, saveName, fullPathLength);
                copyFile(filePath, fullPath);
                FreeVec(fullPath);
            }
        }
        FreeAslRequest(fileReq);
    }
}
MakeHook(SaveSpeechCopyButtonClickedHook, SaveSpeechCopyButtonClickedFunc);

HOOKPROTONHNONP(GenerateSpeechTextButtonClickedFunc, void) {
    STRPTR text = duplicateEditorText(speechInputTextEditor);
    struct Conversation *conversation;
    struct ConversationNode *userMessage;
    struct ChatRequestSettings chatSettings;
    struct json_object **responses;
    UTF8 *textUTF8;
    UTF8 *content;
    STRPTR displayText;

    if (text == NULL || strlen(text) == 0) {
        if (text != NULL)
            FreeVec(text);
        return;
    }

    conversation = newConversation();
    if (conversation == NULL) {
        FreeVec(text);
        displayError(STRING_ERROR_MEMORY_CONVERSATION_NODE);
        return;
    }
    textUTF8 = CodesetsUTF8Create(CSA_SourceCodeset, (Tag)systemCodeset,
                                  CSA_Source, (Tag)text, CSA_MapForeignChars,
                                  TRUE, TAG_DONE);
    userMessage = addTextToConversation(
        conversation, textUTF8 != NULL ? textUTF8 : (UTF8 *)text, "user");
    if (textUTF8 != NULL)
        CodesetsFreeA(textUTF8, NULL);
    if (userMessage == NULL ||
        !copyChatFiles(&userMessage->files, &pendingSpeechFiles)) {
        displayError(STRING_ERROR_MEMORY_CONVERSATION_NODE);
        freeConversation(conversation);
        FreeVec(text);
        return;
    }

    beginCancellableRequest();
    setRequestInterfaceBusy(TRUE);
    updateStatusBar(STRING_SENDING_MESSAGE, yellowPen);
    showLoadingBar();
    configGetActiveChatRequestSettings(&chatSettings);
    setConversationSystem(conversation, chatSettings.chatSystem);
    responses = postChatMessageToOpenAI(
        conversation, chatSettings.host, chatSettings.port, chatSettings.useSSL,
        chatSettings.model, chatSettings.apiKey, FALSE, chatSettings.useProxy,
        chatSettings.proxyHost, chatSettings.proxyPort,
        chatSettings.proxyUsesSSL, chatSettings.proxyRequiresAuth,
        chatSettings.proxyUsername, chatSettings.proxyPassword,
        chatSettings.webSearchEnabled, FALSE, FALSE, chatSettings.apiEndpoint,
        chatSettings.apiEndpointUrl, chatSettings.authorizationType,
        chatSettings.customHeaders);
    if (isRequestCancelled()) {
        if (responses != NULL) {
            if (responses[0] != NULL)
                json_object_put(responses[0]);
            FreeVec(responses);
        }
        freeConversation(conversation);
        FreeVec(text);
        finishActiveRequest(FALSE);
        return;
    }
    if (responses == NULL || responses[0] == NULL) {
        if (responses != NULL)
            FreeVec(responses);
        freeConversation(conversation);
        FreeVec(text);
        finishActiveRequest(FALSE);
        displayError(STRING_ERROR_CONNECTING_OPENAI);
        return;
    }
    content = getMessageContentFromJson(responses[0], FALSE, FALSE,
                                        chatSettings.apiEndpoint);
    if (content != NULL && strlen((char *)content) > 0) {
        displayText = CodesetsUTF8ToStr(
            CSA_DestCodeset, (Tag)systemCodeset, CSA_Source, (Tag)content,
            CSA_MapForeignChars, TRUE, TAG_DONE);
        setEditorText(speechInputTextEditor,
                      displayText != NULL ? displayText : (STRPTR)content);
        if (displayText != NULL)
            CodesetsFreeA(displayText, NULL);
        if (speakFileContentsCheckbox != NULL)
            set(speakFileContentsCheckbox, MUIA_Selected, FALSE);
    }
    json_object_put(responses[0]);
    FreeVec(responses);
    freeConversation(conversation);
    FreeVec(text);
    finishActiveRequest(TRUE);
}
MakeHook(GenerateSpeechTextButtonClickedHook,
         GenerateSpeechTextButtonClickedFunc);


HOOKPROTONHNONP(SaveResponseFilesButtonClickedFunc, void) {
    if (currentConversation == NULL ||
        responseFileCount(currentConversation) == 0)
        return;
    struct FileRequester *fileReq =
        AllocAslRequestTags(ASL_FileRequest, TAG_END);
    if (fileReq == NULL)
        return;
    if (!AslRequestTags(fileReq, ASLFR_Window, mainWindow, ASLFR_TitleText,
                        STRING_SAVE_RESPONSE_FILES, ASLFR_DrawersOnly, TRUE,
                        ASLFR_InitialDrawer, "SYS:", TAG_DONE)) {
        FreeAslRequest(fileReq);
        return;
    }

    struct ChatRequestSettings chatSettings;
    configGetActiveChatRequestSettings(&chatSettings);
    ULONG savedCount = 0;
    struct ConversationNode *message;
    for (message =
             (struct ConversationNode *)currentConversation->messages->mlh_Head;
         message->node.mln_Succ != NULL;
         message = (struct ConversationNode *)message->node.mln_Succ) {
        if (strcmp(message->role, "assistant") != 0)
            continue;
        struct ChatFile *file;
        for (file = (struct ChatFile *)message->files.mlh_Head;
             file->node.mln_Succ != NULL;
             file = (struct ChatFile *)file->node.mln_Succ) {
            STRPTR systemName = CodesetsUTF8ToStr(
                CSA_DestCodeset, (Tag)systemCodeset, CSA_Source,
                (Tag)file->name, CSA_MapForeignChars, TRUE, TAG_DONE);
            CONST_STRPTR usableName = chatFileSafeName(
                systemName != NULL ? systemName : (STRPTR)file->name);
            STRPTR destination =
                uniqueChatFileDestination(fileReq->fr_Drawer, usableName);
            if (destination == NULL) {
                if (systemName != NULL)
                    CodesetsFreeA(systemName, NULL);
                continue;
            }

            if (saveChatFileToPath(
                    file, destination, chatSettings.host, chatSettings.port,
                    chatSettings.useSSL, chatSettings.apiKey,
                    chatSettings.useProxy, chatSettings.proxyHost,
                    chatSettings.proxyPort, chatSettings.proxyUsesSSL,
                    chatSettings.proxyRequiresAuth, chatSettings.proxyUsername,
                    chatSettings.proxyPassword, chatSettings.apiEndpointUrl,
                    chatSettings.authorizationType,
                    chatSettings.customHeaders) == RETURN_OK)
                savedCount++;
            FreeVec(destination);
            if (systemName != NULL)
                CodesetsFreeA(systemName, NULL);
        }
    }
    FreeAslRequest(fileReq);
    if (savedCount > 0) {
        UBYTE statusMessage[128];
        snprintf(statusMessage, sizeof(statusMessage),
                 STRING_RESPONSE_FILES_SAVED_FORMAT, savedCount);
        updateStatusBar(statusMessage, greenPen);
        saveConversations();
    }
}
MakeHook(SaveResponseFilesButtonClickedHook,
         SaveResponseFilesButtonClickedFunc);

HOOKPROTONHNONP(SendChatDeferredFunc, void) {
    struct ChatRequestSettings chatSettings;
    if (requestInterfaceBusy)
        return;
    if (isSpeechPlaying()) {
        stopSpeech();
        updatePlayButton();
        return;
    }
    configGetActiveChatRequestSettings(&chatSettings);
    if (chatSettings.authorizationType != AUTHORIZATION_TYPE_NONE &&
        (chatSettings.apiKey == NULL || strlen(chatSettings.apiKey) == 0)) {
        displayError(STRING_ERROR_NO_API_KEY);
        return;
    }
    sendChatMessage();
}
MakeHook(SendChatDeferredHook, SendChatDeferredFunc);

HOOKPROTONHNONP(SendMessageButtonClickedFunc, void) {
    if (abortIfRequestBusy())
        return;
    queueRequestHook(&SendChatDeferredHook);
}
MakeHook(SendMessageButtonClickedHook, SendMessageButtonClickedFunc);

HOOKPROTONHNONP(NewImageButtonClickedFunc, void) {
    currentImage = NULL;
    freeChatFiles(&pendingImageFiles);
    updateReferenceImageControls();
    set(imageInputTextEditor, MUIA_Disabled, FALSE);
    set(createImageButton, MUIA_Disabled, FALSE);
    set(openImageButton, MUIA_Disabled, TRUE);
    set(saveImageCopyButton, MUIA_Disabled, TRUE);
    set(editImageButton, MUIA_Disabled, TRUE);
    if (isAROS) {
        set(imageInputTextEditor, MUIA_String_Contents, "");
    } else {
        DoMethod(imageInputTextEditor, MUIM_TextEditor_ClearText);
    }
    DoMethod(imageInputTextEditor, MUIM_GoActive);
    DoMethod(imageViewGroup, MUIM_Group_InitChange);
    DoMethod(imageViewGroup, OM_REMMEMBER, imageView);
    MUI_DisposeObject(imageView);
    // clang-format off
    imageView = RectangleObject, MUIA_Frame, MUIV_Frame_ImageButton, End;
    // clang-format on
    DoMethod(imageViewGroup, OM_ADDMEMBER, imageView);
    DoMethod(imageViewGroup, MUIM_Group_MoveMember, imageView, 0);
    DoMethod(imageViewGroup, MUIM_Group_ExitChange);
}
MakeHook(NewImageButtonClickedHook, NewImageButtonClickedFunc);

HOOKPROTONHNONP(DeleteImageButtonClickedFunc, void) {
    struct GeneratedImage *entry;
    DoMethod(imageListObject, MUIM_NList_GetEntry, MUIV_NList_GetEntry_Active,
             &entry);
    if (entry != NULL && entry->filePath != NULL) {
#if defined(__AMIGAOS3__) || defined(__MORPHOS__)
        DeleteFile(entry->filePath);
#else
        Delete(entry->filePath);
#endif
    }
    DoMethod(imageListObject, MUIM_NList_Remove, MUIV_NList_Remove_Active);
    DoMethod(imageListObject, MUIM_NList_Select, MUIV_NList_Select_All,
             MUIV_NList_Select_Off, NULL);
    set(openImageButton, MUIA_Disabled, TRUE);
    set(saveImageCopyButton, MUIA_Disabled, TRUE);
    set(editImageButton, MUIA_Disabled, TRUE);
    currentImage = NULL;
    if (isAROS) {
        set(imageInputTextEditor, MUIA_String_Contents, "");
    } else {
        DoMethod(imageInputTextEditor, MUIM_TextEditor_ClearText);
    }
    DoMethod(imageViewGroup, MUIM_Group_InitChange);
    DoMethod(imageViewGroup, OM_REMMEMBER, imageView);
    MUI_DisposeObject(imageView);
    // clang-format off
    imageView = RectangleObject, MUIA_Frame, MUIV_Frame_ImageButton, End;
    // clang-format on
    DoMethod(imageViewGroup, OM_ADDMEMBER, imageView);
    DoMethod(imageViewGroup, MUIM_Group_MoveMember, imageView, 0);
    DoMethod(imageViewGroup, MUIM_Group_ExitChange);
    saveImages();
}
MakeHook(DeleteImageButtonClickedHook, DeleteImageButtonClickedFunc);

/* Start a fresh prompt with the image on screen attached as its reference, so
 * that "make the sky darker" is one click away from a picture that just came
 * back. The list selection is dropped -- the next Create adds a new entry
 * rather than replacing this one -- but the preview stays put so the user can
 * still see what they are working from. */
HOOKPROTONHNONP(EditImageButtonClickedFunc, void) {
    struct GeneratedImage *source = currentImage;
    if (source == NULL || source->filePath == NULL)
        return;
    CONST_STRPTR name = FilePart(source->filePath);
    if (name == NULL || *name == '\0')
        name = source->filePath;
    freeChatFiles(&pendingImageFiles);
    addPendingFile(&pendingImageFiles, source->filePath, name, TRUE);
    updateReferenceImageControls();

    currentImage = NULL;
    if (isAROS) {
        set(imageInputTextEditor, MUIA_String_Contents, "");
    } else {
        DoMethod(imageInputTextEditor, MUIM_TextEditor_ClearText);
    }
    set(imageInputTextEditor, MUIA_Disabled, FALSE);
    set(createImageButton, MUIA_Disabled, FALSE);
    set(openImageButton, MUIA_Disabled, TRUE);
    set(saveImageCopyButton, MUIA_Disabled, TRUE);
    set(editImageButton, MUIA_Disabled, TRUE);
    DoMethod(imageInputTextEditor, MUIM_GoActive);
}
MakeHook(EditImageButtonClickedHook, EditImageButtonClickedFunc);

static BOOL isStringInList(CONST_STRPTR str, CONST_STRPTR *list) {
    if (str == NULL || list == NULL)
        return FALSE;
    for (UBYTE i = 0; list[i] != NULL; i++) {
        if (strcmp(str, list[i]) == 0)
            return TRUE;
    }
    return FALSE;
}

HOOKPROTONHNONP(CreateImageDeferredFunc, void) {
    if (requestInterfaceBusy)
        return;
    struct ImageRequestSettings imageSettings;
    configGetActiveImageRequestSettings(&imageSettings);
    CONST_STRPTR apiKey = imageSettings.apiKey;
    if (imageSettings.authorizationType != AUTHORIZATION_TYPE_NONE &&
        (apiKey == NULL || strlen(apiKey) == 0)) {
        displayError(STRING_ERROR_NO_API_KEY);
        return;
    }
    set(openImageButton, MUIA_Disabled, TRUE);
    set(saveImageCopyButton, MUIA_Disabled, TRUE);
    set(editImageButton, MUIA_Disabled, TRUE);
    beginCancellableRequest();
    setRequestInterfaceBusy(TRUE);
    showLoadingBar();
    updateStatusBar(STRING_PREPARING_REQUEST, yellowPen);
    STRPTR text;
    if (isAROS) {
        get(imageInputTextEditor, MUIA_String_Contents, &text);
    } else {
        text = DoMethod(imageInputTextEditor, MUIM_TextEditor_ExportText);
    }
    // Remove trailing newline characters
    while (text[strlen(text) - 1] == '\n') {
        text[strlen(text) - 1] = '\0';
    }

    /* Keep legacy image model enum for history + size selection. */
    ImageModel imageModel = configGetImageModel();
    ImageSize imageSize;
    switch (imageModel) {
    case DALL_E_2:
        imageSize = configGetImageSizeDallE2();
        break;
    case DALL_E_3:
        imageSize = configGetImageSizeDallE3();
        break;
    case GPT_IMAGE_1:
    case GPT_IMAGE_1_MINI:
    case GPT_IMAGE_1_5:
        imageSize = configGetImageSizeGptImage1();
        break;
    default:
        imageSize = configGetImageSizeGptImage1();
        break;
    }
    struct json_object *response = postImageCreationRequestToOpenAI(
        text, imageSettings.host, imageSettings.port, imageSettings.useSSL,
        imageSettings.apiEndpointUrl, imageSettings.authorizationType,
        imageSettings.customHeaders, imageSettings.model, imageSize, apiKey,
        configGetProxyEnabled(), configGetProxyHost(), configGetProxyPort(),
        configGetProxyUsesSSL(), configGetProxyRequiresAuth(),
        configGetProxyUsername(), configGetProxyPassword(),
        configGetImageFormat(), imageSettings.imageApiEndpoint,
        &pendingImageFiles);

    if (response == NULL) {
        if (!isRequestCancelled())
            displayError(STRING_ERROR_CONNECTION);
        finishActiveRequest(FALSE);
        if (!isAROS) {
            FreeVec(text);
        }
        return;
    }
    struct json_object *error;

    if (json_object_object_get_ex(response, "error", &error) &&
        !json_object_is_type(error, json_type_null)) {
        struct json_object *message = json_object_object_get(error, "message");
        UTF8 *messageString = json_object_get_string(message);
        STRPTR messageStringSystemEncoded = CodesetsUTF8ToStr(
            CSA_DestCodeset, (Tag)systemCodeset, CSA_Source, (Tag)messageString,
            CSA_MapForeignChars, TRUE, TAG_DONE);
        if (messageStringSystemEncoded != NULL) {
            displayError(messageStringSystemEncoded);
            CodesetsFreeA(messageStringSystemEncoded, NULL);
        } else {
            struct json_object *type = json_object_object_get(error, "type");
            UTF8 *typeString = json_object_get_string(type);
            if (typeString != NULL) {
                if (strcmp(typeString, "invalid_request_error") == 0) {
                    displayError(STRING_ERROR_INVALID_REQUEST);
                } else {
                    displayError(typeString);
                }
            }
        }
        finishActiveRequest(FALSE);
        json_object_put(response);
        if (!isAROS) {
            FreeVec(text);
        }
        return;
    }

    struct json_object *dataArrayObj = NULL;
    struct array_list *data = NULL;
    struct json_object *dataObject = NULL;
    STRPTR b64 = NULL;

    if (json_object_object_get_ex(response, "data", &dataArrayObj) &&
        json_object_is_type(dataArrayObj, json_type_array)) {
        data = json_object_get_array(dataArrayObj);
    }
    if (data != NULL && data->length > 0) {
        dataObject = (struct json_object *)data->array[0];
    }
    if (dataObject != NULL && json_object_is_type(dataObject, json_type_object)) {
        struct json_object *b64Obj = NULL;
        if (json_object_object_get_ex(dataObject, "b64_json", &b64Obj)) {
            b64 = (STRPTR)json_object_get_string(b64Obj);
        }
    }
    if (b64 == NULL) {
        displayError(STRING_ERROR_CONNECTION);
        json_object_put(response);
        set(createImageButton, MUIA_Disabled, FALSE);
        set(newImageButton, MUIA_Disabled, FALSE);
        set(deleteImageButton, MUIA_Disabled, FALSE);
        set(imageInputTextEditor, MUIA_Disabled, FALSE);
        if (!isAROS) { FreeVec(text); }
        return;
    }

    STRPTR imageData;
    ULONG b64Len = strlen(b64);
    CodesetsDecodeB64(CSA_B64SourceString, (Tag)b64, CSA_B64SourceLen,
                      (Tag)b64Len, CSA_B64DestPtr, (Tag)&imageData, TAG_DONE);
    if (imageData == NULL) {
        displayError(STRING_ERROR_INVALID_BASE64);
        finishActiveRequest(FALSE);
        json_object_put(response);
        return;
    }

    LONG data_len = (b64Len * 3) / 4;
    while (b64Len > 0 && b64[--b64Len] == '=')
        data_len--;

    CreateDir("AMIGAGPT:images");
    /* Try to match file extension to actual bytes; fallback to user preference.
     */
    STRPTR imageFormat;
    if (data_len >= 8 && imageData[0] == 0x89 && imageData[1] == 0x50 &&
        imageData[2] == 0x4E && imageData[3] == 0x47 && imageData[4] == 0x0D &&
        imageData[5] == 0x0A && imageData[6] == 0x1A && imageData[7] == 0x0A) {
        imageFormat = "png";
    } else if (data_len >= 3 && imageData[0] == 0xFF && imageData[1] == 0xD8 &&
               imageData[2] == 0xFF) {
        imageFormat = "jpg";
    } else {
        ImageFormat cfgFmt = configGetImageFormat();
        if (cfgFmt != IMAGE_FORMAT_NULL && IMAGE_FORMAT_NAMES[cfgFmt] != NULL) {
            imageFormat = IMAGE_FORMAT_NAMES[cfgFmt];
        } else {
            imageFormat = "png";
        }
    }

    // Generate unique ID for the image
    UBYTE fullPath[35] = "";
    UBYTE id[11] = "";
    CONST_STRPTR idChars = "abcdefghijklmnopqrstuvwxyz0123456789";
    srand(time(NULL));
    for (UBYTE i = 0; i < 9; i++) {
        id[i] = idChars[rand() % strlen(idChars)];
    }
    snprintf(fullPath, sizeof(fullPath), "AMIGAGPT:images/%s.%s", id,
             imageFormat);

    if (imageData == NULL || data_len <= 0) {
        displayError(STRING_ERROR_MEMORY_CONVERSATION_NODE);
        json_object_put(response);
        set(createImageButton, MUIA_Disabled, FALSE);
        set(newImageButton, MUIA_Disabled, FALSE);
        set(deleteImageButton, MUIA_Disabled, FALSE);
        set(imageInputTextEditor, MUIA_Disabled, FALSE);
        if (!isAROS) { FreeVec(text); }
        return;
    }

    FILE *file = fopen(fullPath, "wb");
    if (file != NULL) {
        fwrite(imageData, 1, data_len, file);
        fclose(file);
    }
    CodesetsFreeA(imageData, NULL);

    json_object_put(response);

    WORD imageWidth, imageHeight;
    switch (imageSize) {
    case IMAGE_SIZE_NULL:
        /* "None" means don't send size; image may still be returned. */
        imageWidth = 1024;
        imageHeight = 1024;
        break;
    case IMAGE_SIZE_256x256:
        imageWidth = 256;
        imageHeight = 256;
        break;
    case IMAGE_SIZE_512x512:
        imageWidth = 512;
        imageHeight = 512;
        break;
    case IMAGE_SIZE_1024x1024:
        imageWidth = 1024;
        imageHeight = 1024;
        break;
    case IMAGE_SIZE_1792x1024:
        imageWidth = 1792;
        imageHeight = 1024;
        break;
    case IMAGE_SIZE_1024x1792:
        imageWidth = 1024;
        imageHeight = 1792;
        break;
    case IMAGE_SIZE_1024x1536:
        imageWidth = 1024;
        imageHeight = 1536;
        break;
    case IMAGE_SIZE_1536x1024:
        imageWidth = 1536;
        imageHeight = 1024;
        break;
    case IMAGE_SIZE_AUTO:
        imageWidth = 1024;
        imageHeight = 1024;
        break;
    default:
        displayError(STRING_ERROR_INVALID_IMAGE_SIZE);
        imageWidth = 256;
        imageHeight = 256;
        break;
    }

    updateStatusBar(STRING_GENERATING_IMAGE_NAME, 7);
    struct ChatRequestSettings nameSettings;
    configGetActiveChatRequestSettings(&nameSettings);
    nameSettings.webSearchEnabled = FALSE;
    /* Image title generation must use a chat-capable model. If the user has
     * (accidentally) selected an image model as their chat model, fall back to
     * a sane chat default for the active provider to avoid bad requests. */
    CONST_STRPTR titleModel = nameSettings.model;
    if (titleModel != NULL && strlen(titleModel) > 0) {
        BOOL isImageModel = FALSE;
        if (hostIsOpenAICompatible(nameSettings.host,
                                   nameSettings.apiEndpointUrl)) {
            isImageModel = isStringInList(titleModel, OPENAI_IMAGE_MODELS);
        } else if (nameSettings.host != NULL &&
                   strcmp(nameSettings.host,
                          "generativelanguage.googleapis.com") == 0) {
            isImageModel = isStringInList(titleModel, GEMINI_IMAGE_MODELS);
        } else if (nameSettings.host != NULL &&
                   strcmp(nameSettings.host, "api.x.ai") == 0) {
            isImageModel = isStringInList(titleModel, GROK_IMAGE_MODELS);
        }
        if (isImageModel) {
            if (hostIsOpenAICompatible(nameSettings.host,
                                       nameSettings.apiEndpointUrl)) {
                titleModel = DEFAULT_OPENAI_CHAT_MODEL;
            } else if (nameSettings.host != NULL &&
                       strcmp(nameSettings.host,
                              "generativelanguage.googleapis.com") == 0) {
                titleModel = DEFAULT_GEMINI_CHAT_MODEL;
            } else if (nameSettings.host != NULL &&
                       strcmp(nameSettings.host, "api.x.ai") == 0) {
                titleModel = DEFAULT_GROK_CHAT_MODEL;
            }
        }
    }
    UTF8 *textUTF8 = CodesetsUTF8Create(CSA_SourceCodeset, (Tag)systemCodeset,
                                        CSA_Source, (Tag)text, TAG_DONE);
    struct Conversation *imageNameConversation = newConversation();
    addTextToConversation(imageNameConversation, textUTF8, "user");
    CodesetsFreeA(textUTF8, NULL);
    addTextToConversation(
        imageNameConversation,
        "generate a short title for this image and don't enclose the "
        "title "
        "in quotes or prefix the response with anything",
        "user");
    struct json_object **responses = postChatMessageToOpenAI(
        imageNameConversation, nameSettings.host, nameSettings.port,
        nameSettings.useSSL, titleModel, nameSettings.apiKey, FALSE,
        nameSettings.useProxy, nameSettings.proxyHost, nameSettings.proxyPort,
        nameSettings.proxyUsesSSL, nameSettings.proxyRequiresAuth,
        nameSettings.proxyUsername, nameSettings.proxyPassword, FALSE, FALSE,
        FALSE, nameSettings.apiEndpoint, nameSettings.apiEndpointUrl,
        nameSettings.authorizationType, nameSettings.customHeaders);

    struct GeneratedImage *generatedImage =
        AllocVec(sizeof(struct GeneratedImage), MEMF_ANY);
    if (responses == NULL) {
        displayError(STRING_ERROR_GENERATING_IMAGE_NAME);
    } else if (responses[0] != NULL) {
        if (json_object_object_get_ex(responses[0], "error", &error) &&
            !json_object_is_type(error, json_type_null)) {
            finishActiveRequest(FALSE);
            json_object_put(responses[0]);
            FreeVec(responses);
            if (!isAROS) {
                FreeVec(text);
            }
            freeConversation(imageNameConversation);
            return;
        }
        /* Some providers (e.g. Claude with web search) may return text in
         * multiple response objects. Concatenate all text parts. */
        ULONG combinedLen = 1;
        UWORD ri = 0;
        struct json_object *r = NULL;
        STRPTR combined = NULL;
        while ((r = responses[ri++]) != NULL) {
            UTF8 *part = getMessageContentFromJson(r, FALSE, FALSE,
                                                   nameSettings.apiEndpoint);
            if (part != NULL)
                combinedLen += strlen(part) + 1;
        }
        combined = AllocVec(combinedLen, MEMF_ANY | MEMF_CLEAR);
        if (combined == NULL) {
            combined = (STRPTR) "";
        } else {
            ri = 0;
            while ((r = responses[ri++]) != NULL) {
                UTF8 *part = getMessageContentFromJson(
                    r, FALSE, FALSE, nameSettings.apiEndpoint);
                if (part != NULL && strlen(part) > 0) {
                    strncat(combined, part, combinedLen - strlen(combined) - 1);
                }
            }
        }
        generatedImage->name =
            AllocVec(strlen(combined) + 1, MEMF_ANY | MEMF_CLEAR);
        if (generatedImage->name != NULL) {
            strncpy(generatedImage->name, combined, strlen(combined));
        }
        updateStatusBar(STRING_READY, 5);
        ri = 0;
        while ((r = responses[ri++]) != NULL) {
            json_object_put(r);
        }
        FreeVec(responses);
        if (combined != NULL && combined != (STRPTR) "") {
            FreeVec(combined);
        }
    } else {
        generatedImage->name = dupStringAlloc(id);
        updateStatusBar(STRING_READY, 5);
        if (responses != NULL) {
            FreeVec(responses);
        }
        displayError("Failed to generate image name. Using ID instead.");
    }
    freeConversation(imageNameConversation);

    generatedImage->filePath = dupStringAlloc(fullPath);
    generatedImage->prompt = dupStringAlloc(text);
    if (generatedImage->name == NULL || generatedImage->filePath == NULL ||
        generatedImage->prompt == NULL) {
        if (generatedImage->name != NULL) {
            FreeVec(generatedImage->name);
        }
        if (generatedImage->filePath != NULL) {
            FreeVec(generatedImage->filePath);
        }
        if (generatedImage->prompt != NULL) {
            FreeVec(generatedImage->prompt);
        }
        FreeVec(generatedImage);
        set(createImageButton, MUIA_Disabled, FALSE);
        set(newImageButton, MUIA_Disabled, FALSE);
        set(deleteImageButton, MUIA_Disabled, FALSE);
        set(imageInputTextEditor, MUIA_Disabled, FALSE);
        if (!isAROS) {
            FreeVec(text);
        }
        displayError(STRING_ERROR_GENERATING_IMAGE_NAME);
        return;
    }
    generatedImage->imageModel = configGetImageModel();
    generatedImage->width = imageWidth;
    generatedImage->height = imageHeight;
    DoMethod(imageListObject, MUIM_NList_InsertSingle, generatedImage,
             MUIV_NList_Insert_Top);
    DoMethod(imageListObject, MUIM_NList_SetActive, 0, NULL);
    currentImage = generatedImage;
    ImageRowClickedFunc();

    saveImages();
    finishActiveRequest(TRUE);

    if (!isAROS) {
        FreeVec(text);
    }
}
MakeHook(CreateImageDeferredHook, CreateImageDeferredFunc);

HOOKPROTONHNONP(CreateImageButtonClickedFunc, void) {
    if (abortIfRequestBusy())
        return;
    queueRequestHook(&CreateImageDeferredHook);
}
MakeHook(CreateImageButtonClickedHook, CreateImageButtonClickedFunc);

HOOKPROTONHNONP(OpenImageButtonClickedFunc, void) {
    // Get screen dimensions from main window
    struct Screen *currentScreen;
    get(mainWindowObject, MUIA_Window_Screen, &currentScreen);
    WORD screenWidth = currentScreen ? currentScreen->Width : 640;
    WORD screenHeight = currentScreen ? currentScreen->Height : 480;

    // Calculate the maximum size for the image
    LONG maxImageWidth = (screenWidth * 90) / 100;   // 90% of screen width
    LONG maxImageHeight = (screenHeight * 90) / 100; // 90% of screen height

    // Variables to hold the new dimensions
    LONG newWidth = 0;
    LONG newHeight = 0;

    if (currentImage != NULL) {
        if (currentImage->width > currentImage->height) {
            // Landscape: fit width and calculate height maintaining aspect
            // ratio
            newWidth = maxImageWidth;
            newHeight =
                (currentImage->height * maxImageWidth) / currentImage->width;

            // Ensure height does not exceed maximum
            if (newHeight > maxImageHeight) {
                newHeight = maxImageHeight;
                newWidth = (currentImage->width * maxImageHeight) /
                           currentImage->height;
            }
        } else {
            // Portrait or square: fit height and calculate width
            // maintaining aspect ratio
            newHeight = maxImageHeight;
            newWidth =
                (currentImage->width * maxImageHeight) / currentImage->height;

            // Ensure width does not exceed maximum
            if (newWidth > maxImageWidth) {
                newWidth = maxImageWidth;
                newHeight = (currentImage->height * maxImageWidth) /
                            currentImage->width;
            }
        }

        // Open the image with calculated dimensions
        openImage(currentImage, newWidth, newHeight);
    } else {
        displayError(STRING_ERROR_NO_IMAGE_SELECTED);
    }
}
MakeHook(OpenImageButtonClickedHook, OpenImageButtonClickedFunc);

HOOKPROTONHNONP(SaveImageCopyButtonClickedFunc, void) {
    if (currentImage == NULL)
        return;
    STRPTR filePath = currentImage->filePath;
    STRPTR fileExtension = strrchr(filePath, '.');
    if (fileExtension == NULL) {
        fileExtension = ".png";
    }
    UBYTE fileName[11] = "";
    snprintf(fileName, sizeof(fileName), "image%s\0", fileExtension);
    struct FileRequester *fileReq =
        AllocAslRequestTags(ASL_FileRequest, TAG_END);
    if (fileReq != NULL) {
        if (AslRequestTags(fileReq, ASLFR_Window, mainWindow, ASLFR_TitleText,
                           STRING_SAVE_IMAGE_COPY, ASLFR_InitialFile, fileName,
                           ASLFR_InitialDrawer, "SYS:", ASLFR_DoSaveMode, TRUE,
                           TAG_DONE)) {
            STRPTR savePath = fileReq->fr_Drawer;
            STRPTR saveName = fileReq->fr_File;
            UWORD fullPathLength = strlen(savePath) + strlen(saveName) + 2;
            STRPTR fullPath = AllocVec(fullPathLength, MEMF_CLEAR);
            if (fullPath != NULL) {
                snprintf(fullPath, fullPathLength, "%s", savePath);
                AddPart(fullPath, saveName, fullPathLength);
                copyFile(filePath, fullPath);
                FreeVec(fullPath);
            }
        }
        FreeAslRequest(fileReq);
    }
}
MakeHook(SaveImageCopyButtonClickedHook, SaveImageCopyButtonClickedFunc);

HOOKPROTONHNONP(ConfigureForScreenFunc, void) {
    struct Screen *currentScreen;

    if (mainWindowObject == NULL || app == NULL) {
        return;
    }

    get(mainWindowObject, MUIA_Window_Screen, &currentScreen);

    if (currentScreen) {
        // Release old pens if they exist
        if (redPen)
            ReleasePen(currentScreen->ViewPort.ColorMap, redPen);
        if (greenPen)
            ReleasePen(currentScreen->ViewPort.ColorMap, greenPen);
        if (bluePen)
            ReleasePen(currentScreen->ViewPort.ColorMap, bluePen);
        if (yellowPen)
            ReleasePen(currentScreen->ViewPort.ColorMap, yellowPen);

        // Allocate new pens for current screen
        redPen = ObtainBestPen(currentScreen->ViewPort.ColorMap, 0xFFFFFFFF, 0,
                               0, OBP_Precision, PRECISION_GUI, TAG_DONE);
        greenPen =
            ObtainBestPen(currentScreen->ViewPort.ColorMap, 0, 0xBBBBBBBB, 0,
                          OBP_Precision, PRECISION_GUI, TAG_DONE);
        bluePen =
            ObtainBestPen(currentScreen->ViewPort.ColorMap, 0, 0, 0xFFFFFFFF,
                          OBP_Precision, PRECISION_GUI, TAG_DONE);
        yellowPen = ObtainBestPen(currentScreen->ViewPort.ColorMap, 0xFFFFFFFF,
                                  0xFFFFFFFF, 0, OBP_Precision, PRECISION_GUI,
                                  TAG_DONE);
    }

    const UBYTE BUTTON_LABEL_BUFFER_SIZE = 64;
    STRPTR buttonLabelText = AllocVec(BUTTON_LABEL_BUFFER_SIZE, MEMF_ANY);
    snprintf(buttonLabelText, BUTTON_LABEL_BUFFER_SIZE, "\33c\33P[%ld]+ %s\0",
             greenPen, STRING_NEW_CHAT);
    set(newChatButton, MUIA_Text_Contents, buttonLabelText);
    snprintf(buttonLabelText, BUTTON_LABEL_BUFFER_SIZE, "\33c\33P[%ld]- %s\0",
             redPen, STRING_DELETE_CHAT);
    set(deleteChatButton, MUIA_Text_Contents, buttonLabelText);
    snprintf(buttonLabelText, BUTTON_LABEL_BUFFER_SIZE, "\33c\33P[%ld]+ %s\0",
             greenPen, STRING_NEW_IMAGE);
    set(newImageButton, MUIA_Text_Contents, buttonLabelText);
    snprintf(buttonLabelText, BUTTON_LABEL_BUFFER_SIZE, "\33c\33P[%ld]+ %s\0",
             greenPen, STRING_NEW_PHRASE);
    set(newSpeechButton, MUIA_Text_Contents, buttonLabelText);
    snprintf(buttonLabelText, BUTTON_LABEL_BUFFER_SIZE, "\33c\33P[%ld]- %s\0",
             redPen, STRING_DELETE_PHRASE);
    set(deleteSpeechButton, MUIA_Text_Contents, buttonLabelText);
    snprintf(buttonLabelText, BUTTON_LABEL_BUFFER_SIZE, "\33c\33P[%ld]- %s\0",
             redPen, STRING_DELETE_IMAGE);
    set(deleteImageButton, MUIA_Text_Contents, buttonLabelText);
    setActionButtonsStopMode(requestInterfaceBusy);
    applySpeechWaveformPens();
    FreeVec(buttonLabelText);
    SetAttrs(mainWindowObject, MUIA_Window_ActiveObject, chatInputTextEditor,
             TAG_DONE);

    set(openImageButton, MUIA_Disabled, TRUE);
    set(saveImageCopyButton, MUIA_Disabled, TRUE);
    set(editImageButton, MUIA_Disabled, TRUE);

    updateStatusBar(STRING_READY, greenPen);
}
MakeHook(ConfigureForScreenHook, ConfigureForScreenFunc);

/**
 * Initialize the style stack.
 * @param s : the style stack
 */
static void initStyleStack(StyleStack *s) { s->top = -1; }

/**
 * Push a style onto the stack.
 * @param s : the style stack
 * @param style : the style to push
 * @return true if the style was pushed, false if the stack is full
 */
static BOOL pushStyle(StyleStack *s, StyleType style) {
    if (s->top >= (MAX_STYLE_STACK - 1)) {
        return FALSE;
    }
    s->stack[++s->top] = style;
    return TRUE;
}

/**
 * Pop the top style from the stack.
 * @param s : the style stack
 * @param style : the style to pop
 * @return true if the top style was 'style' and was popped, false otherwise
 */
static BOOL popStyle(StyleStack *s, StyleType style) {
    if (s->top < 0) {
        return FALSE;
    }
    if (s->stack[s->top] == style) {
        s->top--;
        return TRUE;
    }
    return FALSE;
}

/**
 * Check if the stack is currently 'style' on top.
 * @return true if stack is currently 'style' on top
 */
static BOOL isTopStyle(const StyleStack *s, StyleType style) {
    if (s->top < 0)
        return FALSE;
    return (s->stack[s->top] == style);
}

static BOOL stackHasBold(const StyleStack *s) {
    int i;

    for (i = 0; i <= s->top; i++) {
        if (s->stack[i] == STYLE_BOLD) {
            return TRUE;
        }
    }
    return FALSE;
}

static void popBoldRun(StyleStack *s) {
    while (s->top >= 0 && s->stack[s->top] != STYLE_BOLD) {
        s->top--;
    }
    if (s->top >= 0) {
        s->top--;
    }
}

/**
 * Output MUI escape codes for turning on/off styles:
 *   \033b = bold on
 *   \033i = italic on
 *   \033u = underline on
 *   \033n = normal (off)
 *
 * @param out : output buffer
 * @param outSize : size of the output buffer
 * @param style : style to turn on
 */
static void outputStyleOn(STRPTR out, size_t outSize, StyleType style) {
    switch (style) {
    case STYLE_BOLD:
        strbufAppend(out, (ULONG)outSize, "\033b");
        break;
    case STYLE_ITALIC:
        strbufAppend(out, (ULONG)outSize, "\033i");
        break;
    case STYLE_UNDERLINE:
        strbufAppend(out, (ULONG)outSize, "\033u");
        break;
    }
}

/*
 * Output MUI escape code to turn off all styles.
 * @param out : output buffer
 * @param outSize : size of the output buffer
 */
static void outputStyleOff(STRPTR out, size_t outSize) {
    /*
       MCC_NList's docs say \033n sets the soft style back to normal,
       thus turning off bold/italic/underline.
    */
    strbufAppend(out, (ULONG)outSize, "\033n");
}

/*
 * Attempt to parse the next formatting marker:
 *  - "**" => toggles bold
 *  - "*"  => toggles italic
 *  - "__" => toggles underline
 *  - "_"  => toggles italic (some Markdown variants do this),
 *            but we'll skip that unless you truly want single underscore
 *            for italic. We'll assume only * for italic, __ for underline.
 *
 * @param input : the input string
 * @param pos : current position in the input string
 * @param len : length of the input string
 * @param foundStyle : output parameter for the style found
 * @return :
 *   0 if no recognised marker
 *   2 if recognized a 2-char marker (e.g., "**", "__")
 *   1 if recognized a single-char marker (e.g., "*")
 *
 * Also outputs which style it corresponds to in 'foundStyle'.
 */
static UBYTE parseMarker(CONST_STRPTR input, size_t pos, size_t len,
                         StyleType *foundStyle, const StyleStack *stack) {
    if (pos >= len) {
        return 0;
    }

    // Check for "__" (underline)
    if (pos + 1 < len && input[pos] == '_' && input[pos + 1] == '_') {
        *foundStyle = STYLE_UNDERLINE;
        return 2;
    }
    if (pos + 1 < len && input[pos] == '*' && input[pos + 1] == '*') {
        if (stackHasBold(stack)) {
            if (!chatMdBoldDoubleStarCanClose((const char *)input, (ULONG)pos,
                                              (ULONG)len)) {
                return 0;
            }
        } else if (!chatMdBoldDoubleStarCanOpen((const char *)input, (ULONG)pos,
                                                (ULONG)len)) {
            return 0;
        }
        *foundStyle = STYLE_BOLD;
        return 2;
    }
    if (input[pos] == '*') {
        BOOL closing = isTopStyle(stack, STYLE_ITALIC);

        if (closing ? chatMdItalicStarCanClose((const char *)input, (ULONG)pos,
                                               (ULONG)len)
                    : chatMdItalicStarCanOpen((const char *)input, (ULONG)pos,
                                              (ULONG)len)) {
            *foundStyle = STYLE_ITALIC;
            return 1;
        }
        return 0;
    }
    return 0;
}

/**
 * Convert a (mini-)Markdown string with possible nesting of
 *   **bold**, *italic*, __underline__
 * to MUI/NList escape codes with a proper stack approach.
 *
 * - Supports nesting, e.g. **bold *italic** inside**
 * - Supports escaping:
 *      \*, \_, \\, so they're inserted literally.
 *
 * @param input the input string
 * @return a newly allocated string. Caller must FreeVec() it.
 */
static STRPTR convertMarkdownFormattingToMUI(CONST_STRPTR input) {
    if (!input)
        return NULL;

    // Heuristic for output expansion: each marker can expand
    // to ~4 chars ("\033b" or "\033n"), so let's be generous.
    size_t inLen = strlen(input);
    size_t outCap = inLen * 6 + 128;
    STRPTR out = AllocVec(outCap, MEMF_ANY | MEMF_CLEAR);
    if (!out)
        return NULL;

    StyleStack styleStack;
    initStyleStack(&styleStack);

    size_t i = 0;
    while (i < inLen) {
        // 1) Check for escapes: "\"
        if (input[i] == '\\') {
            // If we have a next char, output it literally and skip
            if (i + 1 < inLen) {
                // Next char is literal
                size_t outLen = strlen(out);
                if (outLen < outCap - 2) {
                    out[outLen] = input[i + 1];
                    out[outLen + 1] = '\0';
                }
                i += 2;
            } else {
                // Just a trailing backslash alone:
                // output it or ignore, your choice. We'll just ignore it.
                i++;
            }
            continue;
        }

        // 2) Try to parse a formatting marker
        StyleType styleFound;
        UBYTE markerLen = parseMarker(input, i, inLen, &styleFound, &styleStack);
        if (markerLen > 0) {
            if (styleFound == STYLE_BOLD) {
                if (stackHasBold(&styleStack)) {
                    popBoldRun(&styleStack);
                    outputStyleOff(out, outCap);
                    for (UWORD s = 0; s <= styleStack.top; s++) {
                        outputStyleOn(out, outCap, styleStack.stack[s]);
                    }
                } else if (pushStyle(&styleStack, STYLE_BOLD)) {
                    outputStyleOn(out, outCap, STYLE_BOLD);
                } else {
                    size_t availableSpace = outCap - strlen(out) - 1;
                    size_t copyLen = (markerLen < availableSpace)
                                         ? markerLen
                                         : availableSpace;

                    UBYTE tempBuf[256];
                    if (copyLen > sizeof(tempBuf) - 1) {
                        copyLen = sizeof(tempBuf) - 1;
                    }

                    memcpy(tempBuf, &input[i], copyLen);
                    tempBuf[copyLen] = '\0';

                    strbufAppend(out, (ULONG)outCap, (STRPTR)tempBuf);
                }
            } else if (isTopStyle(&styleStack, styleFound)) {
                popStyle(&styleStack, styleFound);
                outputStyleOff(out, outCap);

                for (UWORD s = 0; s <= styleStack.top; s++) {
                    outputStyleOn(out, outCap, styleStack.stack[s]);
                }
            } else {
                if (pushStyle(&styleStack, styleFound)) {
                    outputStyleOn(out, outCap, styleFound);
                } else {
                    // We've run out of stack space; treat as literal
                    // We'll just insert the raw marker:
                    size_t availableSpace = outCap - strlen(out) - 1;
                    size_t copyLen = (markerLen < availableSpace)
                                         ? markerLen
                                         : availableSpace;

                    UBYTE tempBuf[256];
                    if (copyLen > sizeof(tempBuf) - 1) {
                        copyLen = sizeof(tempBuf) - 1;
                    }

                    memcpy(tempBuf, &input[i], copyLen);
                    tempBuf[copyLen] = '\0';

                    strbufAppend(out, (ULONG)outCap, (STRPTR)tempBuf);
                }
            }
            i += markerLen;
        } else {
            // 3) Not a marker, just a normal character
            size_t outLen = strlen(out);
            if (outLen < outCap - 2) {
                out[outLen] = input[i];
                out[outLen + 1] = '\0';
            }
            i++;
        }
    }

    // If any styles remain open, we can close them
    while (styleStack.top >= 0) {
        popStyle(&styleStack, styleStack.stack[styleStack.top]);
        outputStyleOff(out, outCap);
        // Re-enable anything that might remain on the stack
        for (int s = 0; s <= styleStack.top; s++) {
            outputStyleOn(out, outCap, styleStack.stack[s]);
        }
    }

    return out;
}

static BOOL ensureChatOutputBufferCapacity(ULONG required) {
    if (chatOutputTextEditorContents == NULL)
        return FALSE;
    if (required <= chatOutputTextEditorContentsCapacity)
        return TRUE;

    ULONG newCapacity = chatOutputTextEditorContentsCapacity;
    while (newCapacity < required) {
        if (newCapacity > 8 * 1024 * 1024) {
            break;
        }
        newCapacity *= 2;
    }
    if (newCapacity < required) {
        return FALSE;
    }

    STRPTR bigger = AllocVec(newCapacity, MEMF_ANY | MEMF_CLEAR);
    if (bigger == NULL) {
        return FALSE;
    }
    strncpy(bigger, chatOutputTextEditorContents,
            chatOutputTextEditorContentsCapacity - 1);
    set(chatOutputTextEditor, MUIA_NFloattext_Text, "");
    FreeVec(chatOutputTextEditorContents);
    chatOutputTextEditorContents = bigger;
    chatOutputTextEditorContentsCapacity = newCapacity;
    return TRUE;
}

static Object *createPromptInputEditor(struct Hook *submitHook, ULONG objectId,
                                       ULONG weight) {
    /* TextFrame + MUII_BACKGROUND: solid editor look (upstream dropped both).
     * Prefer private AmigaGPTTextEditor: Shift+Return (L+R) inserts newline.
     * ActionTextEditor.mcc only remaps IEQUALIFIER_RSHIFT ? Left Shift+Return
     * fails there. Fall back to MCC, then stock TextEditor. */
    // clang-format off
    if (!isAROS && amigaGPTTextEditorClass != NULL) {
        Object *editor = NewObject(
            MUIC_AmigaGPTTextEditor, NULL,
            TextFrame,
            MUIA_Background, MUII_BACKGROUND,
            MUIA_CycleChain, TRUE,
            objectId != 0 ? MUIA_ObjectID : TAG_IGNORE, objectId,
            weight != 0 ? MUIA_Weight : TAG_IGNORE, weight,
            MUIA_AmigaGPTTextEditor_SubmitHook, submitHook,
            isAROS ? TAG_DONE : TAG_SKIP, NULL,
#ifndef __MORPHOS__
            MUIA_TextEditor_FixedFont, configGetFixedWidthFonts(),
#endif
            MUIA_TextEditor_ReadOnly, FALSE,
            MUIA_TextEditor_TabSize, 4,
            MUIA_TextEditor_Rows, 3,
            MUIA_TextEditor_ExportHook, MUIV_TextEditor_ExportHook_EMail,
            TAG_DONE);
        if (editor != NULL)
            return editor;
    }
    if (!isAROS) {
        Object *editor = MUI_NewObject(
            MUIC_ActionTextEditor,
            TextFrame,
            MUIA_Background, MUII_BACKGROUND,
            MUIA_CycleChain, TRUE,
            objectId != 0 ? MUIA_ObjectID : TAG_IGNORE, objectId,
            weight != 0 ? MUIA_Weight : TAG_IGNORE, weight,
            MUIA_ActionTextEditor_SubmitHook, submitHook,
            isAROS ? TAG_DONE : TAG_SKIP, NULL,
#ifndef __MORPHOS__
            MUIA_TextEditor_FixedFont, configGetFixedWidthFonts(),
#endif
            MUIA_TextEditor_ReadOnly, FALSE,
            MUIA_TextEditor_TabSize, 4,
            MUIA_TextEditor_Rows, 3,
            MUIA_TextEditor_ExportHook, MUIV_TextEditor_ExportHook_EMail,
            TAG_DONE);
        if (editor != NULL)
            return editor;
    }
    return MUI_NewObject(
        isAROS ? MUIC_BetterString : MUIC_TextEditor,
        TextFrame,
        MUIA_Background, MUII_BACKGROUND,
        MUIA_CycleChain, TRUE,
        objectId != 0 ? MUIA_ObjectID : TAG_IGNORE, objectId,
        weight != 0 ? MUIA_Weight : TAG_IGNORE, weight,
        isAROS ? TAG_DONE : TAG_SKIP, NULL,
#ifndef __MORPHOS__
        MUIA_TextEditor_FixedFont, configGetFixedWidthFonts(),
#endif
        MUIA_TextEditor_ReadOnly, FALSE,
        MUIA_TextEditor_TabSize, 4,
        MUIA_TextEditor_Rows, 3,
        MUIA_TextEditor_ExportHook, MUIV_TextEditor_ExportHook_EMail,
        TAG_DONE);
    // clang-format on
}

static Object *promptEditorForPage(LONG page) {
    if (page == 0)
        return chatInputTextEditor;
    if (page == 1)
        return imageInputTextEditor;
    return speechInputTextEditor;
}

HOOKPROTONHNONP(ModePageChangedFunc, void) {
    LONG page = 0;
    Object *editor;

    get(modeRegisterGroup, MUIA_Group_ActivePage, &page);
    editor = promptEditorForPage(page);
    if (editor != NULL && mainWindowObject != NULL) {
        set(mainWindowObject, MUIA_Window_ActiveObject, editor);
        DoMethod(editor, MUIM_GoActive);
    }
    updateEditMenuItemsEnabled();
}
MakeHook(ModePageChangedHook, ModePageChangedFunc);

/**
 * Prepare assistant text for NFloattext. Returns a newly AllocVec'd string,
 * or NULL if input is NULL or allocation fails. Caller must FreeVec() when
 * non-NULL.
 */
static STRPTR formatAssistantTextForDisplay(CONST_STRPTR input) {
    STRPTR out;
    size_t len;

    if (input == NULL) {
        return NULL;
    }
    if (!configGetMarkdownFormatting()) {
        len = strlen(input);
        out = AllocVec(len + 1, MEMF_CLEAR);
        if (out == NULL) {
            return NULL;
        }
        if (len > 0) {
            CopyMem(input, out, len);
        }
        out[len] = '\0';
        return out;
    }
    return convertMarkdownFormattingToMUI(input);
}

#ifdef __MORPHOS__

static void chatOutputFillRoleStyles(struct Conversation *conversation,
                                     UBYTE *styles, ULONG bufLen) {
    struct ConversationNode *conversationNode;
    UTF8 *content;
    ULONG contentLen;
    ULONG pos = 0;
    UBYTE styleByte;

    if (styles == NULL || conversation == NULL || bufLen == 0) {
        return;
    }

    memset(styles, 0, bufLen);

    for (conversationNode =
             (struct ConversationNode *)conversation->messages->mlh_Head;
         conversationNode->node.mln_Succ != NULL;
         conversationNode =
             (struct ConversationNode *)conversationNode->node.mln_Succ) {
        content = conversationNodeGetDisplay(conversationNode);
        if (content == NULL) {
            content = (UTF8 *)"";
        }
        contentLen = (ULONG)strlen((const char *)content);

        if (pos > 0) {
            if (pos + 2 > bufLen) {
                return;
            }
            pos += 2;
        }

        styleByte =
            (strcmp(conversationNode->role, "user") == 0) ? (UBYTE)1 : (UBYTE)0;

        if (pos + contentLen > bufLen) {
            if (pos < bufLen) {
                memset(styles + pos, styleByte, bufLen - pos);
            }
            return;
        }
        memset(styles + pos, styleByte, contentLen);
        pos += contentLen;
    }
}

static BOOL mainWindowShuttingDown = FALSE;

BOOL mainWindowMorphosPrepareShutdownActive(void) { return mainWindowShuttingDown; }

void chatOutputUpdateFromBuffer(BOOL preserveViewport) {
    ULONG textLen;
    UBYTE *roleStyles = NULL;

    if (mainWindowShuttingDown || mainWindowObject == NULL ||
        chatOutputTextEditor == NULL || chatOutputTextEditorContents == NULL) {
        return;
    }
#ifdef __MORPHOS__
    /* Recover if a prior send left live-skip stuck (Markdown/hotspots dead). */
    if (!chatSendInProgress) {
        morphosChatLiveScintillaUpdates = FALSE;
    }
#endif

    textLen = (ULONG)strlen(chatOutputTextEditorContents);
    if (textLen == 0) {
        streamLogLifecycle("chatOutputUpdateFromBuffer empty");
        chatOutputScintillaSetUtf8TextWithRoleStyles(chatOutputTextEditor, "", NULL, 0,
                                                     preserveViewport);
#ifdef __MORPHOS__
        chatUserNavClear();
        chatFindScintillaUpdateCounter(chatOutputTextEditor);
#endif
        return;
    }

    if (currentConversation != NULL) {
        roleStyles = (UBYTE *)AllocVec(textLen, MEMF_ANY);
        if (roleStyles != NULL) {
            char *displayText = NULL;
            UBYTE *displayStyles = NULL;

            streamLogLifecycle("chatOutputUpdateFromBuffer roleStyles begin");
            chatOutputFillRoleStyles(currentConversation, roleStyles, textLen);
            streamLogLifecycle("chatOutputUpdateFromBuffer roleStyles done");

#ifdef __MORPHOS__
            {
                /* displayConversation uses RefreshFromList; that must NOT force
                 * raw on large tables -- Markdown menu uses plain Refresh and
                 * formats correctly. Raw only: markdown off, mid-stream flag, CJK. */
                BOOL morphosUseRawRefresh =
                    !configGetMarkdownFormatting() ||
                    morphosChatStreamRawScintillaRefresh ||
                    utf8_contains_cjk(
                        (const UBYTE *)chatOutputTextEditorContents);

                /* SSE live window only -- finishChatStream clears this first
                 * so the final paint can run while chatSendInProgress is set. */
                if (morphosChatLiveScintillaUpdates) {
                    FreeVec(roleStyles);
                    return;
                }

                /* List/final rebuild: skip GOTOPOS; still allow Midi-Markdown. */
                chatOutputScintillaMorphosSkipViewport =
                    chatOutputMorphosListRefreshActive;

                if (morphosUseRawRefresh) {
                    streamLogLifecycle("chatOutput refresh raw path");
                    /* Hotspot scan on multi-KB tables freezes before paint. */
                    if (textLen <= (STREAM_UI_LIVE_SCINTILLA_MAX * 4UL)) {
                        chatOutputScintillaAugmentStyleBytesHotspots(
                            chatOutputTextEditorContents, roleStyles,
                            textLen);
                    }
                    streamLogLifecycle(
                        "chatOutputUpdateFromBuffer scintilla raw begin");
                    chatOutputScintillaForgetMarkdownLinkSpans();
                    chatOutputScintillaSetUtf8TextWithRoleStyles(
                        chatOutputTextEditor, chatOutputTextEditorContents,
                        roleStyles, textLen, preserveViewport);
                    streamLogLifecycle(
                        "chatOutputUpdateFromBuffer scintilla raw done");
                    chatUserNavRebuild(roleStyles, textLen);
                    chatFindScintillaUpdateCounter(chatOutputTextEditor);
                    FreeVec(roleStyles);
                    return;
                }
                if (chatOutputMorphosListRefreshActive) {
                    streamLogLifecycle("chatOutput refresh list markdown path");
                } else {
                    streamLogLifecycle("chatOutput refresh markdown path");
                }
            }
#endif

            displayText = (char *)AllocVec((textLen * 4) + 1, MEMF_ANY);
            displayStyles = (UBYTE *)AllocVec(textLen * 4, MEMF_ANY);

            if (displayText != NULL && displayStyles != NULL) {
                ULONG displayCap = textLen * 4;

                streamLogLifecycle("chatOutputUpdateFromBuffer markdown begin");
                textLen = chatOutputScintillaBuildMidiMarkdownDisplay(
                    chatOutputTextEditorContents, roleStyles, textLen,
                    displayText, displayStyles);
                streamLogLifecycle("chatOutputUpdateFromBuffer markdown done");
                streamLogLifecycle("chatOutputUpdateFromBuffer pipeTables begin");
                textLen = chatOutputScintillaFormatPipeTables(
                    displayText, displayStyles, textLen, displayCap);
                streamLogLifecycle("chatOutputUpdateFromBuffer pipeTables done");
                streamLogLifecycle("chatOutputUpdateFromBuffer scintilla set begin");
                chatOutputScintillaSetUtf8TextWithRoleStyles(
                    chatOutputTextEditor, displayText, displayStyles, textLen,
                    preserveViewport);
                streamLogLifecycle("chatOutputUpdateFromBuffer scintilla set done");
#ifdef __MORPHOS__
                chatUserNavRebuild(displayStyles, textLen);
                chatFindScintillaUpdateCounter(chatOutputTextEditor);
#endif
                FreeVec(displayText);
                FreeVec(displayStyles);
            } else {
                if (displayText != NULL) {
                    FreeVec(displayText);
                }
                if (displayStyles != NULL) {
                    FreeVec(displayStyles);
                }
                streamLogLifecycle("chatOutputUpdateFromBuffer scintilla fallback begin");
                chatOutputScintillaForgetMarkdownLinkSpans();
                chatOutputScintillaAugmentStyleBytesHotspots(
                    chatOutputTextEditorContents, roleStyles, textLen);
                chatOutputScintillaSetUtf8TextWithRoleStyles(
                    chatOutputTextEditor, chatOutputTextEditorContents,
                    roleStyles, textLen, preserveViewport);
                streamLogLifecycle("chatOutputUpdateFromBuffer scintilla fallback done");
#ifdef __MORPHOS__
                chatUserNavRebuild(roleStyles, textLen);
                chatFindScintillaUpdateCounter(chatOutputTextEditor);
#endif
            }

            FreeVec(roleStyles);
            return;
        }
        streamLogLifecycle("chatOutputUpdateFromBuffer roleStyles alloc fail");
    }

    streamLogLifecycle("chatOutputUpdateFromBuffer scintilla plain begin");
    chatOutputScintillaSetUtf8TextWithRoleStyles(
        chatOutputTextEditor, chatOutputTextEditorContents, roleStyles, textLen,
        preserveViewport);
    streamLogLifecycle("chatOutputUpdateFromBuffer scintilla plain done");

    if (roleStyles != NULL) {
        FreeVec(roleStyles);
    }
}

static void clearChatOutputDisplay(void) {
    if (chatOutputTextEditorContents != NULL) {
        chatOutputTextEditorContents[0] = '\0';
    }
    chatOutputUpdateFromBuffer(FALSE);
}

#else /* !__MORPHOS__ */

/* Phase 7c: mouse wheel on chat NFloattext (HandleInput + window EH on same class) */
static struct MUI_CustomClass *chatOutputFloattextClass;
static struct MUI_EventHandlerNode *chatOutputWheelEH;
static BOOL chatOutputWheelEHInstalled;

static LONG chatOutputWheelScrollCommand(struct MUIP_HandleInput *msg) {
    if (msg == NULL) {
        return 0;
    }
    if (msg->muikey == MUIKEY_UP) {
        return MUIV_NList_First_Up;
    }
    if (msg->muikey == MUIKEY_DOWN) {
        return MUIV_NList_First_Down;
    }
    if (msg->muikey == MUIKEY_PAGEUP) {
        return MUIV_NList_First_PageUp;
    }
    if (msg->muikey == MUIKEY_PAGEDOWN) {
        return MUIV_NList_First_PageDown;
    }
    if (msg->imsg != NULL) {
        BOOL shift =
            (msg->imsg->Qualifier &
             (IEQUALIFIER_LSHIFT | IEQUALIFIER_RSHIFT)) != 0;
        UWORD code = msg->imsg->Code;

        if (msg->imsg->Class == IDCMP_RAWKEY ||
            msg->imsg->Class == IECLASS_RAWKEY ||
            msg->imsg->Class == IECLASS_NEWMOUSE) {
            switch (code) {
            case NM_WHEEL_UP:
                return shift ? MUIV_NList_First_PageUp : MUIV_NList_First_Up;
            case NM_WHEEL_DOWN:
                return shift ? MUIV_NList_First_PageDown
                             : MUIV_NList_First_Down;
            default:
                break;
            }
        }
    }
    return 0;
}

SAVEDS static ULONG ChatOutputFloattext_HandleInput(struct IClass *cl, Object *obj,
                                                   struct MUIP_HandleInput *msg) {
    LONG cmd = chatOutputWheelScrollCommand(msg);

    if (cmd != 0) {
        set(obj, MUIA_NList_First, cmd);
        return TRUE;
    }
    return DoSuperMethodA(cl, obj, msg);
}

SAVEDS static ULONG ChatOutputFloattext_HandleEvent(struct IClass *cl, Object *obj,
                                                    struct MUIP_HandleEvent *msg) {
    struct MUIP_HandleInput inputMsg;
    LONG cmd;

    inputMsg.MethodID = MUIM_HandleInput;
    inputMsg.imsg = msg->imsg;
    inputMsg.muikey = msg->muikey;
    cmd = chatOutputWheelScrollCommand(&inputMsg);
    if (cmd != 0) {
        set(obj, MUIA_NList_First, cmd);
        return MUI_EventHandlerRC_Eat;
    }
    return DoSuperMethodA(cl, obj, msg);
}

DISPATCHER(ChatOutputFloattextDispatcher) {
    switch (msg->MethodID) {
    case MUIM_HandleInput:
        return ChatOutputFloattext_HandleInput(cl, obj, (APTR)msg);
    case MUIM_HandleEvent:
        return ChatOutputFloattext_HandleEvent(cl, obj, (APTR)msg);
    }
    return DoSuperMethodA(cl, obj, msg);
}

static LONG createChatOutputFloattextClass(void) {
    if (chatOutputFloattextClass != NULL) {
        return RETURN_OK;
    }
    chatOutputFloattextClass = MUI_CreateCustomClass(
        NULL, MUIC_NFloattext, NULL, 0, ENTRY(ChatOutputFloattextDispatcher));
    if (chatOutputFloattextClass == NULL) {
        return RETURN_ERROR;
    }
    chatOutputFloattextClass->mcc_Class->cl_ID =
        (ClassID) "AmigaGPTChatOutputFloattext";
    return RETURN_OK;
}

static void deleteChatOutputFloattextClass(void) {
    if (chatOutputFloattextClass != NULL) {
        MUI_DeleteCustomClass(chatOutputFloattextClass);
        chatOutputFloattextClass = NULL;
    }
}

static void installChatOutputWheelHandler(void) {
    if (chatOutputWheelEHInstalled || chatOutputTextEditor == NULL ||
        mainWindowObject == NULL || chatOutputFloattextClass == NULL) {
        return;
    }
    if (chatOutputWheelEH == NULL) {
        chatOutputWheelEH = (struct MUI_EventHandlerNode *)AllocVec(
            sizeof(struct MUI_EventHandlerNode), MEMF_PUBLIC | MEMF_CLEAR);
        if (chatOutputWheelEH == NULL) {
            return;
        }
        chatOutputWheelEH->ehn_Class = chatOutputFloattextClass->mcc_Class;
        chatOutputWheelEH->ehn_Object = chatOutputTextEditor;
        chatOutputWheelEH->ehn_Events = IDCMP_RAWKEY;
        chatOutputWheelEH->ehn_Flags = MUI_EHF_GUIMODE;
        chatOutputWheelEH->ehn_Priority = 100;
    }
    DoMethod(mainWindowObject, MUIM_Window_AddEventHandler, chatOutputWheelEH);
    chatOutputWheelEHInstalled = TRUE;
}

void chatOutputWheelShutdown(void) {
    streamLogLifecycle("chatOutputWheelShutdown begin");
    if (chatOutputWheelEHInstalled && mainWindowObject != NULL &&
        chatOutputWheelEH != NULL) {
        DoMethod(mainWindowObject, MUIM_Window_RemEventHandler, chatOutputWheelEH);
    }
    chatOutputWheelEHInstalled = FALSE;
    if (chatOutputWheelEH != NULL) {
        FreeVec(chatOutputWheelEH);
        chatOutputWheelEH = NULL;
    }
    /* Custom class must outlive chatOutputTextEditor until MUI_DisposeObject(app). */
    streamLogLifecycle("chatOutputWheelShutdown done");
}

void chatOutputWheelDisposeClass(void) {
    deleteChatOutputFloattextClass();
}

static Object *newChatOutputFloattextObject(void) {
    if (createChatOutputFloattextClass() == RETURN_OK) {
        return (Object *)NewObject(chatOutputFloattextClass->mcc_Class, NULL,
            MUIA_Font,
            configGetFixedWidthFonts() ? MUIV_NList_Font_Fixed : MUIV_NList_Font,
            MUIA_Frame, MUIV_Frame_Text, MUIA_ContextMenu, NULL,
            MUIA_NFloattext_Text, chatOutputTextEditorContents, TAG_DONE);
    }
    return (Object *)MUI_NewObject(
        MUIC_NFloattext, MUIA_Font,
        configGetFixedWidthFonts() ? MUIV_NList_Font_Fixed : MUIV_NList_Font,
        MUIA_Frame, MUIV_Frame_Text, MUIA_ContextMenu, NULL,
        MUIA_NFloattext_Text, chatOutputTextEditorContents, TAG_DONE);
}

#endif /* !__MORPHOS__ */

#ifdef __MORPHOS__
void chatOutputWheelShutdown(void) {}
void chatOutputWheelDisposeClass(void) {}

void applyFixedWidthFontsSetting(void) {
    applyChatFontSetting();
}

void applyChatFontSetting(void) {
    if (chatOutputTextEditor == NULL || mainWindowIsShuttingDown()) {
        return;
    }
    streamLogLifecycle("chat font apply begin");
    chatOutputScintillaCancelDeferredStyles();
    chatOutputScintillaRefreshFont(chatOutputTextEditor);
    streamLogLifecycle("chat font apply done");
}
#endif

/**
 * Create the main window
 * @return RETURN_OK on success, RETURN_ERROR on failure
 **/
LONG createMainWindow() {
    streamLogBootPhase("createMainWindow start");
    streamLogLifecycle("createMainWindow start");

    if (!pendingChatFilesInitialized) {
        NewList((struct List *)&pendingChatFiles);
        pendingChatFilesInitialized = TRUE;
    }
    if (!pendingImageFilesInitialized) {
        NewList((struct List *)&pendingImageFiles);
        pendingImageFilesInitialized = TRUE;
    }
    if (!pendingSpeechFilesInitialized) {
        NewList((struct List *)&pendingSpeechFiles);
        pendingSpeechFilesInitialized = TRUE;
    }
#ifndef __MORPHOS__
    (void)createChatOutputFloattextClass();
#else
    if (!chatFindScintillaInit()) {
        streamLogLifecycle("createMainWindow chat find init failed");
    }
#endif
    if ((isMUI5 || isMUI39) && createAmigaGPTTextEditor() == RETURN_ERROR) {
        displayError("Could not create custom class.");
    }
    if (mainWindowObject != NULL) {
#ifdef __MORPHOS__
        chatOutputScintillaDetachNotify();
#endif
        MUI_DisposeObject(mainWindowObject);
    }

    chatInputTextEditor = createPromptInputEditor(
        &SendMessageButtonClickedHook, OBJECT_ID_CHAT_INPUT_TEXT_EDITOR, 0);
    imageInputTextEditor = createPromptInputEditor(
        &CreateImageButtonClickedHook, OBJECT_ID_IMAGE_INPUT_TEXT_EDITOR, 80);
    speechInputTextEditor = createPromptInputEditor(
        &GenerateSpeechButtonClickedHook, OBJECT_ID_SPEECH_INPUT_TEXT_EDITOR,
        80);

    createMenu();

    if (chatOutputTextEditorContents == NULL) {
        chatOutputTextEditorContents = AllocVec(
            CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH, MEMF_ANY | MEMF_CLEAR);
        chatOutputTextEditorContentsCapacity =
            CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH;
    }

    pages[0] = STRING_MENU_CHAT;
    pages[1] = STRING_MENU_IMAGE;
    pages[2] = STRING_MENU_SPEECH;

    haveAudioPlayer = FALSE;
    speechWaveform = MUI_NewObject(
        MUIC_AudioPlayer,
        MUIA_Frame, MUIV_Frame_Group,
        MUIA_Background, MUII_SHADOW,
        MUIA_FillArea, FALSE,
        MUIA_Weight, 200,
        TAG_DONE);
    if (speechWaveform != NULL) {
        haveAudioPlayer = TRUE;
    } else {
        speechWaveform = RectangleObject,
            MUIA_Frame, MUIV_Frame_Group,
            MUIA_Weight, 200,
        End;
    }
    if (speechWaveform == NULL)
        speechWaveform = HVSpace;

    streamLogLifecycle("createMainWindow WindowObject begin");
    if ((mainWindowObject = WindowObject,
            MUIA_Window_Title, STRING_APP_NAME,
            MUIA_Window_ID, OBJECT_ID_MAIN_WINDOW,
            MUIA_Window_CloseGadget, TRUE,
            MUIA_Window_SizeGadget, TRUE,
            MUIA_Window_DepthGadget, TRUE,
            MUIA_Window_DragBar, TRUE,
            MUIA_Window_LeftEdge, MUIV_Window_LeftEdge_Centered,
            MUIA_Window_TopEdge, MUIV_Window_TopEdge_Centered,
            MUIA_Window_Width, MUIV_Window_Width_Screen(90),
            MUIA_Window_Height, MUIV_Window_Height_Screen(90),
            MUIA_Window_Menustrip, menuStrip,
            MUIA_Window_SizeRight, TRUE,
            MUIA_Window_UseBottomBorderScroller, FALSE,
            MUIA_Window_UseRightBorderScroller, FALSE,
            MUIA_Window_UseLeftBorderScroller, FALSE,
            WindowContents, VGroup,
                Child, modeRegisterGroup = RegisterGroup(pages),
                    Child, HGroup,
                        Child, VGroup, MUIA_Weight, 30,
                            // New chat button
                            Child, newChatButton = MUI_MakeObject(MUIO_Button, STRING_NEW_CHAT,
                                MUIA_CycleChain, TRUE,
                                MUIA_InputMode, MUIV_InputMode_RelVerify,
                            TAG_DONE),
                            // Delete chat button
                            Child, deleteChatButton = MUI_MakeObject(MUIO_Button, STRING_DELETE_CHAT,
                                MUIA_Background, MUII_FILL,
                                MUIA_CycleChain, TRUE,
                                MUIA_InputMode, MUIV_InputMode_RelVerify,
                            TAG_DONE),
                            // Conversation list
                            Child, conversationListViewObject = NListviewObject,
                                MUIA_CycleChain, 1,
                                MUIA_NListview_NList, conversationListObject = NListObject,
                                    MUIA_ContextMenu, NULL,
                                    MUIA_NList_DefaultObjectOnClick, TRUE,
                                    MUIA_NList_MultiSelect, MUIV_NList_MultiSelect_None,
                                    MUIA_NList_ConstructHook2, &ConstructConversationLI_TextHook,
                                    MUIA_NList_DestructHook2, &DestructConversationLI_TextHook,
                                    MUIA_NList_DisplayHook2, &DisplayConversationLI_TextHook,
                                    MUIA_NList_Format, "BAR MINW=100 MAXW=200",
                                    MUIA_NList_AutoVisible, TRUE,
                                    MUIA_NList_TitleSeparator, FALSE,
                                    MUIA_NList_Title, FALSE,
                                    MUIA_NList_MinColSortable, 0,
                                    MUIA_NList_Imports, MUIV_NList_Imports_All,
                                    MUIA_NList_Exports, MUIV_NList_Exports_All,
                                End,
                            End,
                        End,
                        Child, VGroup,
                            // Chat output text display
                            Child, HGroup, MUIA_VertWeight, 60,
#ifdef __MORPHOS__
                                Child, VGroup,
                                    Child, chatFindScintillaGetBar(),
                                    Child, chatOutputScroller = ScrollgroupObject,
                                    MUIA_Scrollgroup_AutoBars, TRUE,
                                    MUIA_Scrollgroup_Contents,
                                        chatOutputTextEditor = ScintillaObject,
                                        MUIA_Frame, MUIV_Frame_Text,
                                    End,
                                End,
                                End,
#else
                                Child, chatOutputListView = NListviewObject,
                                MUIA_NListview_Horiz_ScrollBar, MUIV_NListview_HSB_None,
                                MUIA_NListview_Vert_ScrollBar, MUIV_NListview_VSB_Auto,
                                MUIA_NListview_NList,
                                    chatOutputTextEditor =
                                        newChatOutputFloattextObject(),
                                End,
#endif
                            End,
                            Child, HGroup,
                                Child, attachFilesButton = MUI_MakeObject(MUIO_Button, STRING_ATTACH_FILES,
                                    MUIA_CycleChain, TRUE,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                TAG_DONE),
                                Child, clearAttachmentsButton = MUI_MakeObject(MUIO_Button, STRING_CLEAR_ATTACHMENTS,
                                    MUIA_CycleChain, TRUE,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                TAG_DONE),
                                Child, attachmentSummaryText = TextObject,
                                    MUIA_Text_Contents, STRING_NO_FILES_ATTACHED,
                                    MUIA_Text_SetMin, FALSE,
                                End,
                                Child, saveResponseFilesButton = MUI_MakeObject(MUIO_Button, STRING_SAVE_RESPONSE_FILES,
                                    MUIA_CycleChain, TRUE,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                TAG_DONE),
                            End,
                            Child, HGroup, MUIA_VertWeight, 20,
                                // Chat input text editor
                                Child, chatInputTextEditor,
                                // Send message button
                                Child, VGroup, MUIA_HorizWeight, 10,
                                    Child, sendMessageButton = MUI_MakeObject(MUIO_Button, STRING_SEND,
                                        MUIA_ObjectID, OBJECT_ID_SEND_MESSAGE_BUTTON,
                                        MUIA_CycleChain, TRUE,
                                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                                    End,
                                End,
                            End,
                        End,
                    End,
                    Child, HGroup,
                        GroupFrame,
                        Child, VGroup,
                            // New image button
                            Child, newImageButton = MUI_MakeObject(MUIO_Button, STRING_NEW_IMAGE,
                                MUIA_CycleChain, TRUE,
                                MUIA_InputMode, MUIV_InputMode_RelVerify,
                            TAG_DONE),
                            // Delete image button
                            Child, deleteImageButton = MUI_MakeObject(MUIO_Button, STRING_DELETE_IMAGE,
                                MUIA_CycleChain, TRUE,
                                MUIA_InputMode, MUIV_InputMode_RelVerify,
                            TAG_DONE),
                            // Image list
                            Child, NListviewObject,
                                MUIA_CycleChain, 1,
                                MUIA_NListview_NList, imageListObject = NListObject,
                                    MUIA_ContextMenu, NULL,
                                    MUIA_NList_DefaultObjectOnClick, TRUE,
                                    MUIA_NList_MultiSelect, MUIV_NList_MultiSelect_None,
                                    MUIA_NList_ConstructHook2, &ConstructImageLI_TextHook,
                                    MUIA_NList_DestructHook2, &DestructImageLI_TextHook,
                                    MUIA_NList_DisplayHook2, &DisplayImageLI_TextHook,
                                    MUIA_NList_Format, "BAR MINW=100 MAXW=200",
                                    MUIA_NList_AutoVisible, TRUE,
                                    MUIA_NList_TitleSeparator, FALSE,
                                    MUIA_NList_Title, FALSE,
                                    MUIA_NList_MinColSortable, 0,
                                    MUIA_NList_Imports, MUIV_NList_Imports_All,
                                    MUIA_NList_Exports, MUIV_NList_Exports_All,
                                End,
                            End,
                        End,
                        Child, imageViewGroup = VGroup, MUIA_Weight, 220,
                            // Image view
                            Child, imageView = RectangleObject,
                                MUIA_Frame, MUIV_Frame_ImageButton,
                            End,
                            Child, HGroup,
                                // Open image button
                                Child, openImageButton = MUI_MakeObject(MUIO_Button, STRING_OPEN_IMAGE,
                                    MUIA_CycleChain, TRUE,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                TAG_DONE),
                                // Save image copy button
                                Child, saveImageCopyButton = MUI_MakeObject(MUIO_Button, STRING_SAVE_IMAGE_COPY,
                                    MUIA_CycleChain, TRUE,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                TAG_DONE),
                                // Edit image button: reuse this image as a reference for a new prompt
                                Child, editImageButton = MUI_MakeObject(MUIO_Button, STRING_EDIT_IMAGE,
                                    MUIA_CycleChain, TRUE,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                TAG_DONE),
                            End,
                            // Reference images sent along with the prompt
                            Child, HGroup,
                                Child, attachReferenceImagesButton = MUI_MakeObject(MUIO_Button, STRING_ATTACH_REFERENCE_IMAGES,
                                    MUIA_CycleChain, TRUE,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                TAG_DONE),
                                Child, clearReferenceImagesButton = MUI_MakeObject(MUIO_Button, STRING_CLEAR_REFERENCE_IMAGES,
                                    MUIA_CycleChain, TRUE,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                TAG_DONE),
                                Child, referenceImageSummaryText = TextObject,
                                    MUIA_Text_Contents, STRING_NO_REFERENCE_IMAGES,
                                    MUIA_Text_SetMin, FALSE,
                                End,
                            End,
                            Child, HGroup,
                                // Image input text editor
                                Child, imageInputTextEditor,
                                Child, VGroup,
                                    MUIA_Weight, 20,
                                    // Create image button
                                    Child, createImageButton = MUI_MakeObject(MUIO_Button, STRING_CREATE_IMAGE,
                                        MUIA_Weight, 20,
                                        MUIA_CycleChain, TRUE,
                                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                                    TAG_DONE),
                                End,
                            End,
                        End,
                    End,
                    Child, HGroup,
                        GroupFrame,
                        Child, VGroup, MUIA_Weight, 30,
                            Child, newSpeechButton = MUI_MakeObject(MUIO_Button, STRING_NEW_PHRASE,
                                MUIA_CycleChain, TRUE,
                                MUIA_InputMode, MUIV_InputMode_RelVerify,
                            TAG_DONE),
                            Child, deleteSpeechButton = MUI_MakeObject(MUIO_Button, STRING_DELETE_PHRASE,
                                MUIA_Background, MUII_FILL,
                                MUIA_CycleChain, TRUE,
                                MUIA_InputMode, MUIV_InputMode_RelVerify,
                            TAG_DONE),
                            Child, NListviewObject,
                                MUIA_CycleChain, 1,
                                MUIA_NListview_NList, speechListObject = NListObject,
                                    MUIA_ContextMenu, NULL,
                                    MUIA_NList_DefaultObjectOnClick, TRUE,
                                    MUIA_NList_MultiSelect, MUIV_NList_MultiSelect_None,
                                    MUIA_NList_ConstructHook2, &ConstructSpeechLI_TextHook,
                                    MUIA_NList_DestructHook2, &DestructSpeechLI_TextHook,
                                    MUIA_NList_DisplayHook2, &DisplaySpeechLI_TextHook,
                                    MUIA_NList_Format, "BAR MINW=100 MAXW=200",
                                    MUIA_NList_AutoVisible, TRUE,
                                    MUIA_NList_TitleSeparator, FALSE,
                                    MUIA_NList_Title, FALSE,
                                    MUIA_NList_MinColSortable, 0,
                                    MUIA_NList_Imports, MUIV_NList_Imports_All,
                                    MUIA_NList_Exports, MUIV_NList_Exports_All,
                                End,
                            End,
                        End,
                        Child, VGroup,
                            Child, speechProfileInfo = TextObject,
                                MUIA_Frame, MUIV_Frame_Group,
                                MUIA_Text_Contents, STRING_NO_SPEECH_SELECTED,
                                MUIA_Text_SetMin, FALSE,
                                MUIA_Weight, 20,
                            End,
                            Child, speechWaveform,
                            Child, HGroup,
                                Child, regenerateSpeechButton = MUI_MakeObject(MUIO_Button, STRING_REGENERATE,
                                    MUIA_CycleChain, TRUE,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                TAG_DONE),
                                Child, saveSpeechCopyButton = MUI_MakeObject(MUIO_Button, STRING_SAVE_SPEECH_COPY,
                                    MUIA_CycleChain, TRUE,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                TAG_DONE),
                            End,
                            Child, HGroup,
                                Child, attachSpeechFilesButton = MUI_MakeObject(MUIO_Button, STRING_ATTACH_FILES,
                                    MUIA_CycleChain, TRUE,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                TAG_DONE),
                                Child, clearSpeechFilesButton = MUI_MakeObject(MUIO_Button, STRING_CLEAR_ATTACHMENTS,
                                    MUIA_CycleChain, TRUE,
                                    MUIA_InputMode, MUIV_InputMode_RelVerify,
                                TAG_DONE),
                                Child, speechAttachmentSummaryText = TextObject,
                                    MUIA_Text_Contents, STRING_NO_FILES_ATTACHED,
                                    MUIA_Text_SetMin, FALSE,
                                End,
                            End,
                            Child, HGroup,
                                Child, speakFileContentsCheckbox = MUI_MakeObject(MUIO_Checkmark, NULL),
                                Child, LLabel(STRING_SPEAK_CONTENTS_OF_FILES),
                                Child, HSpace(0),
                            End,
                            Child, HGroup,
                                Child, speechInputTextEditor,
                                Child, VGroup,
                                    MUIA_Weight, 20,
                                    Child, generateSpeechButton = MUI_MakeObject(MUIO_Button, STRING_GENERATE_SPEECH,
                                        MUIA_CycleChain, TRUE,
                                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                                    TAG_DONE),
                                    Child, generateSpeechTextButton = MUI_MakeObject(MUIO_Button, STRING_GENERATE_TEXT_WITH_AI,
                                        MUIA_CycleChain, TRUE,
                                        MUIA_InputMode, MUIV_InputMode_RelVerify,
                                    TAG_DONE),
                                End,
                            End,
                        End,
                    End,
                End,
                // Status bar
                Child, statusBar = TextObject,
                    MUIA_Background, MUII_SHADOW,
                    MUIA_Text_Contents, STRING_READY,
                End,
                // Loading bar: blank page while idle, busy meter page while a
                // request is in flight, so it no longer stays frozen on its
                // last position once a request finishes.
                Child, loadingBarGroup = PageGroup, MUIA_VertWeight, 10,
                    MUIA_MaxHeight, 20,
                    MUIA_Group_ActivePage, 0,
                    Child, HVSpace,
                    Child, loadingBar = BusyObject,
                        MUIA_Busy_Speed, MUIV_Busy_Speed_Off,
                    End,
                End,
            End,
        End) == NULL) {
        streamLogLifecycle("createMainWindow WindowObject fail");
        streamLogStartupPhase("createMainWindow fail");
        // clang-format on
        displayError(STRING_ERROR_MAIN_WINDOW);
        return RETURN_ERROR;
    }
    streamLogLifecycle("createMainWindow WindowObject ok");

    get(mainWindowObject, MUIA_Window, &mainWindow);
#ifdef __MORPHOS__
    streamLogLifecycle("createMainWindow codeBlocksViewerSetAslParentWindow begin");
    codeBlocksViewerSetAslParentWindow(mainWindow);
    streamLogLifecycle("createMainWindow codeBlocksViewerSetAslParentWindow done");
    streamLogLifecycle("createMainWindow chat scintilla prime begin");
    chatOutputScintillaPrimeViewer(chatOutputTextEditor);
    streamLogLifecycle("createMainWindow chat scintilla prime done");
#endif

    streamLogLifecycle("createMainWindow OM_ADDMEMBER begin");
    DoMethod(app, OM_ADDMEMBER, mainWindowObject);
    streamLogLifecycle("createMainWindow OM_ADDMEMBER done");

    addMainWindowActions();

#ifdef __MORPHOS__
    chatFindScintillaSetContext(mainWindowObject, chatOutputTextEditor);
    chatFindScintillaWireToApp(app);
#endif

    /* Once before open; second Load after loadConversations broke NList on restart. */
    streamLogLifecycle("createMainWindow Application_Load begin");
    DoMethod(app, MUIM_Application_Load, MUIV_Application_Load_ENVARC);
    streamLogLifecycle("createMainWindow Application_Load done");
#ifdef __MORPHOS__
    /* ENVARC must not reopen code viewer before Scintilla is fully ready. */
    if (codeBlocksWindowObject != NULL) {
        streamLogLifecycle("createMainWindow codeBlocksWindow close begin");
        set(codeBlocksWindowObject, MUIA_Window_Open, FALSE);
        streamLogLifecycle("createMainWindow codeBlocksWindow close done");
    }
    streamLogLifecycle("createMainWindow chat scintilla finish deferred");
#endif
    set(mainWindowObject, MUIA_Window_Open, TRUE);
#ifdef __MORPHOS__
    DoMethod(mainWindowObject, MUIM_Window_ToFront, NULL);
#endif
    streamLogBootPhase("main window open");
    streamLogLifecycle("createMainWindow window open done");
    addMenuActions();
    
    // Allocate pens after window is opened
    struct Screen *currentScreen;
    get(mainWindowObject, MUIA_Window_Screen, &currentScreen);
    if (currentScreen) {
        redPen = ObtainBestPen(currentScreen->ViewPort.ColorMap, 0xFFFFFFFF, 0,
                               0, OBP_Precision, PRECISION_GUI, TAG_DONE);
        greenPen =
            ObtainBestPen(currentScreen->ViewPort.ColorMap, 0, 0xBBBBBBBB, 0,
                          OBP_Precision, PRECISION_GUI, TAG_DONE);
        bluePen =
            ObtainBestPen(currentScreen->ViewPort.ColorMap, 0, 0, 0xFFFFFFFF,
                          OBP_Precision, PRECISION_GUI, TAG_DONE);
        yellowPen = ObtainBestPen(currentScreen->ViewPort.ColorMap, 0xFFFFFFFF,
                                  0xFFFFFFFF, 0, OBP_Precision, PRECISION_GUI,
                                  TAG_DONE);

        // Now set up the button labels with colors
        const UBYTE BUTTON_LABEL_BUFFER_SIZE = 64;
        STRPTR buttonLabelText = AllocVec(BUTTON_LABEL_BUFFER_SIZE, MEMF_ANY);
        snprintf(buttonLabelText, BUTTON_LABEL_BUFFER_SIZE,
                 "\33c\33P[%ld]+ %s\0", greenPen, STRING_NEW_CHAT);
        set(newChatButton, MUIA_Text_Contents, buttonLabelText);
        snprintf(buttonLabelText, BUTTON_LABEL_BUFFER_SIZE,
                 "\33c\33P[%ld]- %s\0", redPen, STRING_DELETE_CHAT);
        set(deleteChatButton, MUIA_Text_Contents, buttonLabelText);
        snprintf(buttonLabelText, BUTTON_LABEL_BUFFER_SIZE,
                 "\33c\33P[%ld]+ %s\0", greenPen, STRING_NEW_PHRASE);
        set(newSpeechButton, MUIA_Text_Contents, buttonLabelText);
        snprintf(buttonLabelText, BUTTON_LABEL_BUFFER_SIZE,
                 "\33c\33P[%ld]- %s\0", redPen, STRING_DELETE_PHRASE);
        set(deleteSpeechButton, MUIA_Text_Contents, buttonLabelText);
        snprintf(buttonLabelText, BUTTON_LABEL_BUFFER_SIZE,
                 "\33c\33P[%ld]+ %s\0", greenPen, STRING_NEW_IMAGE);
        set(newImageButton, MUIA_Text_Contents, buttonLabelText);
        snprintf(buttonLabelText, BUTTON_LABEL_BUFFER_SIZE,
                 "\33c\33P[%ld]- %s\0", redPen, STRING_DELETE_IMAGE);
        set(deleteImageButton, MUIA_Text_Contents, buttonLabelText);
        setActionButtonsStopMode(FALSE);
        applySpeechWaveformPens();
        FreeVec(buttonLabelText);

        // Force a layout refresh to ensure buttons display correctly
        DoMethod(mainWindowObject, MUIM_Group_InitChange);
        DoMethod(mainWindowObject, MUIM_Group_ExitChange);
    }
    
    set(openImageButton, MUIA_Disabled, TRUE);
    set(saveImageCopyButton, MUIA_Disabled, TRUE);
    set(editImageButton, MUIA_Disabled, TRUE);
    updateAttachmentControls();
    updateReferenceImageControls();
    updateResponseFileControl();
    updateSpeechControls();

    updateStatusBar(STRING_READY, greenPen);

    streamLogBootPhase("loadConversations call");
    streamLogLifecycle("createMainWindow loadConversations begin");
    loadConversations();
    streamLogBootPhase("loadConversations done");
    streamLogLifecycle("createMainWindow loadConversations done");
    restoreLastSelectedConversation();
    loadImages();
    loadSpeechHistory();
    streamLogBootPhase("createMainWindow ok");
    streamLogLifecycle("createMainWindow ok");

#ifndef __MORPHOS__
    installChatOutputWheelHandler();
#endif

    return RETURN_OK;
}

static void mainWindowReleasePens(void) {
    struct Screen *currentScreen = NULL;

    if (mainWindowObject == NULL) {
        redPen = greenPen = bluePen = yellowPen = 0;
        return;
    }

    get(mainWindowObject, MUIA_Window_Screen, &currentScreen);
    if (currentScreen != NULL) {
        if (redPen) {
            ReleasePen(currentScreen->ViewPort.ColorMap, redPen);
        }
        if (greenPen) {
            ReleasePen(currentScreen->ViewPort.ColorMap, greenPen);
        }
        if (bluePen) {
            ReleasePen(currentScreen->ViewPort.ColorMap, bluePen);
        }
        if (yellowPen) {
            ReleasePen(currentScreen->ViewPort.ColorMap, yellowPen);
        }
    }
    redPen = greenPen = bluePen = yellowPen = 0;
}

static void mainWindowEmptyNList(Object *list) {
    if (list == NULL) {
        return;
    }
    set(list, MUIA_NList_Quiet, TRUE);
    DoMethod(list, MUIM_NList_Clear);
    set(list, MUIA_NList_Quiet, FALSE);
}

void mainWindowPrepareShutdown(void) {
    streamLogLifecycle("mainWindowPrepareShutdown begin");
    streamLogShutdownPhase("prepare shutdown begin");
    mainWindowShuttingDown = TRUE;
    mainWindowSignalQuit();
    openAIChatStreamRequestCancel();
    saveLastSelectedConversationName(currentConversation);
    currentConversation = NULL;
    currentImage = NULL;

#ifdef __MORPHOS__
    morphosConversationSelectEnabled = FALSE;
    conversationRowPending = NULL;
    codeBlocksViewerBeginShutdown();
    chatOutputScintillaCancelPendingCodeblockOpen();
    chatOutputScintillaCancelDeferredStyles();
    chatOutputRefreshPending = FALSE;
    chatOutputRefreshFromList = FALSE;
    morphosFlushPendingPushMethods();
    /* Close code viewer + drop chat notifies while main window is still open. */
    codeBlocksViewerCloseWindow();
    chatFindScintillaHide();
    chatUserNavClear();
    chatOutputScintillaQuiesceForShutdown(chatOutputTextEditor);
    chatOutputScintillaDetachNotify();
    /* No SCI_CLEARALL/SETTEXT on quit — can freeze MorphOS after styled large docs. */
    chatOutputScintillaForgetMarkdownLinkSpans();
#endif

    /* Pens must be released while the window/screen is still valid. */
    mainWindowReleasePens();

    mainWindowEmptyNList(conversationListObject);
    mainWindowEmptyNList(imageListObject);

    if (mainWindowObject != NULL) {
        set(mainWindowObject, MUIA_Window_Open, FALSE);
        DoMethod(mainWindowObject, MUIM_KillNotify, MUIA_Window_Screen);
        DoMethod(mainWindowObject, MUIM_KillNotify, MUIA_Window_CloseRequest);
    }
    streamLogLifecycle("mainWindowPrepareShutdown done");
    streamLogShutdownPhase("prepare shutdown done");
}

void mainWindowInvalidateAfterShutdown(void) {
    mainWindowShuttingDown = FALSE;
#ifdef __MORPHOS__
    morphosStartupDeferredDone = FALSE;
    morphosConversationSelectEnabled = FALSE;
    conversationRowPending = NULL;
    chatOutputRefreshPending = FALSE;
#endif
    mainWindow = NULL;
    mainWindowObject = NULL;
    newChatButton = NULL;
    deleteChatButton = NULL;
    sendMessageButton = NULL;
    chatInputTextEditor = NULL;
#ifndef __MORPHOS__
    chatOutputListView = NULL;
#endif
#ifdef __MORPHOS__
    chatOutputScroller = NULL;
#endif
    chatOutputTextEditor = NULL;
    statusBar = NULL;
    conversationListObject = NULL;
    conversationListViewObject = NULL;
    loadingBar = NULL;
    imageInputTextEditor = NULL;
    createImageButton = NULL;
    newImageButton = NULL;
    deleteImageButton = NULL;
    imageListObject = NULL;
    imageView = NULL;
    imageViewGroup = NULL;
    openImageButton = NULL;
    saveImageCopyButton = NULL;
    modeRegisterGroup = NULL;
    currentConversation = NULL;
    currentImage = NULL;
}

/**
 * Add actions to the main window
 **/
static void addMainWindowActions() {
    DoMethod(newChatButton, MUIM_Notify, MUIA_Pressed, FALSE, newChatButton, 2,
             MUIM_CallHook, &NewChatButtonClickedHook);
    DoMethod(deleteChatButton, MUIM_Notify, MUIA_Pressed, FALSE,
             MUIV_Notify_Application, 3, MUIM_CallHook,
             &DeleteChatButtonClickedHook, MUIV_TriggerValue);
    DoMethod(sendMessageButton, MUIM_Notify, MUIA_Pressed, FALSE,
             sendMessageButton, 2, MUIM_CallHook,
             &SendMessageButtonClickedHook);
    DoMethod(attachFilesButton, MUIM_Notify, MUIA_Pressed, FALSE,
             attachFilesButton, 2, MUIM_CallHook,
             &AttachFilesButtonClickedHook);
    DoMethod(clearAttachmentsButton, MUIM_Notify, MUIA_Pressed, FALSE,
             clearAttachmentsButton, 2, MUIM_CallHook,
             &ClearAttachmentsButtonClickedHook);
    DoMethod(saveResponseFilesButton, MUIM_Notify, MUIA_Pressed, FALSE,
             saveResponseFilesButton, 2, MUIM_CallHook,
             &SaveResponseFilesButtonClickedHook);
    DoMethod(createImageButton, MUIM_Notify, MUIA_Pressed, FALSE,
             createImageButton, 2, MUIM_CallHook,
             &CreateImageButtonClickedHook);
    DoMethod(openImageButton, MUIM_Notify, MUIA_Pressed, FALSE, openImageButton,
             2, MUIM_CallHook, &OpenImageButtonClickedHook);
    DoMethod(saveImageCopyButton, MUIM_Notify, MUIA_Pressed, FALSE,
             saveImageCopyButton, 2, MUIM_CallHook,
             &SaveImageCopyButtonClickedHook);
    DoMethod(editImageButton, MUIM_Notify, MUIA_Pressed, FALSE, editImageButton,
             2, MUIM_CallHook, &EditImageButtonClickedHook);
    DoMethod(attachReferenceImagesButton, MUIM_Notify, MUIA_Pressed, FALSE,
             attachReferenceImagesButton, 2, MUIM_CallHook,
             &AttachReferenceImagesButtonClickedHook);
    DoMethod(clearReferenceImagesButton, MUIM_Notify, MUIA_Pressed, FALSE,
             clearReferenceImagesButton, 2, MUIM_CallHook,
             &ClearReferenceImagesButtonClickedHook);
    DoMethod(conversationListObject, MUIM_Notify, MUIA_NList_EntryClick,
             MUIV_EveryTime, MUIV_Notify_Window, 3, MUIM_CallHook,
             &ConversationRowClickedHook, MUIV_TriggerValue);
    DoMethod(newImageButton, MUIM_Notify, MUIA_Pressed, FALSE, newImageButton,
             2, MUIM_CallHook, &NewImageButtonClickedHook);
    DoMethod(deleteImageButton, MUIM_Notify, MUIA_Pressed, FALSE,
             MUIV_Notify_Application, 3, MUIM_CallHook,
             &DeleteImageButtonClickedHook, MUIV_TriggerValue);
    DoMethod(imageListObject, MUIM_Notify, MUIA_NList_EntryClick,
             MUIV_EveryTime, MUIV_Notify_Window, 3, MUIM_CallHook,
             &ImageRowClickedHook, MUIV_TriggerValue);
    DoMethod(newSpeechButton, MUIM_Notify, MUIA_Pressed, FALSE, newSpeechButton,
             2, MUIM_CallHook, &NewSpeechButtonClickedHook);
    DoMethod(deleteSpeechButton, MUIM_Notify, MUIA_Pressed, FALSE,
             MUIV_Notify_Application, 3, MUIM_CallHook,
             &DeleteSpeechButtonClickedHook, MUIV_TriggerValue);
    DoMethod(generateSpeechButton, MUIM_Notify, MUIA_Pressed, FALSE,
             generateSpeechButton, 2, MUIM_CallHook,
             &GenerateSpeechButtonClickedHook);
    DoMethod(regenerateSpeechButton, MUIM_Notify, MUIA_Pressed, FALSE,
             regenerateSpeechButton, 2, MUIM_CallHook,
             &RegenerateSpeechButtonClickedHook);
    if (haveAudioPlayer && speechWaveform != NULL)
        set(speechWaveform, MUIA_AudioPlayer_HasSpeech, FALSE);
    DoMethod(saveSpeechCopyButton, MUIM_Notify, MUIA_Pressed, FALSE,
             saveSpeechCopyButton, 2, MUIM_CallHook,
             &SaveSpeechCopyButtonClickedHook);
    DoMethod(generateSpeechTextButton, MUIM_Notify, MUIA_Pressed, FALSE,
             generateSpeechTextButton, 2, MUIM_CallHook,
             &GenerateSpeechTextButtonClickedHook);
    DoMethod(attachSpeechFilesButton, MUIM_Notify, MUIA_Pressed, FALSE,
             attachSpeechFilesButton, 2, MUIM_CallHook,
             &AttachSpeechFilesButtonClickedHook);
    DoMethod(clearSpeechFilesButton, MUIM_Notify, MUIA_Pressed, FALSE,
             clearSpeechFilesButton, 2, MUIM_CallHook,
             &ClearSpeechFilesButtonClickedHook);
    DoMethod(speechListObject, MUIM_Notify, MUIA_NList_EntryClick,
             MUIV_EveryTime, MUIV_Notify_Window, 3, MUIM_CallHook,
             &SpeechRowClickedHook, MUIV_TriggerValue);
    DoMethod(speakFileContentsCheckbox, MUIM_Notify, MUIA_Selected,
             MUIV_EveryTime, speakFileContentsCheckbox, 2, MUIM_CallHook,
             &SpeechTextChangedHook);
    DoMethod(modeRegisterGroup, MUIM_Notify, MUIA_Group_ActivePage,
             MUIV_EveryTime, modeRegisterGroup, 2, MUIM_CallHook,
             &ModePageChangedHook);
    if (isAROS) {
        DoMethod(speechInputTextEditor, MUIM_Notify, MUIA_String_Contents,
                 MUIV_EveryTime, speechInputTextEditor, 2, MUIM_CallHook,
                 &SpeechTextChangedHook);
    } else {
        DoMethod(speechInputTextEditor, MUIM_Notify, MUIA_TextEditor_HasChanged,
                 TRUE, speechInputTextEditor, 2, MUIM_CallHook,
                 &SpeechTextChangedHook);
    }
    DoMethod(mainWindowObject, MUIM_Notify, MUIA_Window_CloseRequest, TRUE,
             MUIV_Notify_Application, 2, MUIM_Application_ReturnID,
             MUIV_Application_ReturnID_Quit);
}

static void streamUiResetRefreshClock(void) {
    streamUiLastRefreshValid = FALSE;
}

static BOOL streamUiShouldRefresh(UWORD chunkCount) {
    struct timeval now;

    if (chunkCount == 1) {
        gettimeofday(&streamUiLastRefresh, NULL);
        streamUiLastRefreshValid = TRUE;
        return TRUE;
    }
    if (!streamUiLastRefreshValid) {
        gettimeofday(&streamUiLastRefresh, NULL);
        streamUiLastRefreshValid = TRUE;
        return TRUE;
    }
    gettimeofday(&now, NULL);
    {
        long elapsedMs =
            (long)(now.tv_sec - streamUiLastRefresh.tv_sec) * 1000L +
            (long)(now.tv_usec - streamUiLastRefresh.tv_usec) / 1000L;

        if (elapsedMs >= STREAM_UI_MIN_REFRESH_MS) {
            streamUiLastRefresh = now;
            return TRUE;
        }
    }
    return FALSE;
}

static void streamUiFlushChatDisplay(void) {
#ifdef __MORPHOS__
    /*
     * Any live Scintilla during send (markdown/docswap/styles per SSE batch)
     * eventually hard-freezes MorphOS. Keep the UTF-8 buffer only; paint once
     * after the stream ends (finishChatStream / displayConversation).
     */
    if (morphosChatLiveScintillaUpdates) {
        return;
    }
    chatOutputUpdateFromBuffer(FALSE);
#else
    set(chatOutputTextEditor, MUIA_NFloattext_Text,
        chatOutputTextEditorContents);
    set(chatOutputListView, MUIA_NList_First, MUIV_NList_First_Bottom);
#endif
}

/** Speak only UTF-8 not yet spoken (system TTS); avoids full-buffer convert. */
static void speakStreamUtf8Tail(STRPTR receivedMessage, ULONG *utf8Spoken) {
    ULONG totalLen;
    STRPTR tailSystem;

    if (receivedMessage == NULL || utf8Spoken == NULL) {
        return;
    }
    totalLen = strlen(receivedMessage);
    if (*utf8Spoken >= totalLen) {
        return;
    }
    tailSystem = CodesetsUTF8ToStr(CSA_DestCodeset, (Tag)systemCodeset, CSA_Source,
                                   (Tag)(receivedMessage + *utf8Spoken),
                                   CSA_MapForeignChars, TRUE, TAG_DONE);
    if (tailSystem != NULL && tailSystem[0] != '\0') {
        speakText(tailSystem, NULL, NULL);
        CodesetsFreeA(tailSystem, NULL);
    }
    *utf8Spoken = totalLen;
}

/**
 * Append one validated UTF-8 piece to the live chat view and receivedMessage.
 */
static void appendAssistantStreamText(STRPTR piece, STRPTR receivedMessage,
                                    UWORD *wordNumber, ULONG *speechUtf8Index) {
    size_t pieceLen;

    if (piece == NULL || piece[0] == '\0') {
        return;
    }

    pieceLen = strlen(piece);
    strbufAppend(receivedMessage, READ_BUFFER_LENGTH, piece);

#ifdef __MORPHOS__
    strbufAppend(chatOutputTextEditorContents,
                 CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH, piece);
#else
    {
        STRPTR formattedMessageSystemEncoded = CodesetsUTF8ToStr(
            CSA_DestCodeset, (Tag)systemCodeset, CSA_Source, (Tag)piece,
            CSA_MapForeignChars, TRUE, TAG_DONE);

        if (formattedMessageSystemEncoded != NULL) {
            strbufAppend(chatOutputTextEditorContents,
                         CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH,
                         formattedMessageSystemEncoded);
            CodesetsFreeA(formattedMessageSystemEncoded, NULL);
        }
    }
#endif

    ++(*wordNumber);
    if (*wordNumber == 1) {
        streamLogLifecycle("chat stream first chunk");
    }
    /*
     * OS3/OS4: markdown only after stream (displayConversation); avoids full-buffer
     * convertMarkdownFormattingToMUI on every chunk.
     * MorphOS: live refresh stays raw UTF-8 while morphosChatStreamRawScintillaRefresh
     * (Handlungsanweisung §5 / R3 — no Midi-Markdown or link-parse per chunk).
     */
    if (streamUiShouldRefresh(*wordNumber)) {
        streamUiFlushChatDisplay();
        /* Network TTS shares the chat SSL socket — never speak mid-stream.
         * CJK mid-stream local TTS also stalls the UI on MorphOS. */
        if (configGetSpeechEnabled() &&
            !speechSystemUsesNetwork(configGetSpeechSystem()) &&
            !utf8_contains_cjk((const UBYTE *)receivedMessage)) {
            speakStreamUtf8Tail(receivedMessage, speechUtf8Index);
        }
    }
}

static void appendValidatedUtf8Chunk(struct UTF8StreamBuffer *stream,
                                     const STRPTR chunk,
                                     STRPTR receivedMessage, UWORD *wordNumber,
                                     ULONG *speechUtf8Index) {
    UBYTE piece[2048];
    ULONG taken;

    if (chunk == NULL || chunk[0] == '\0') {
        return;
    }

    if (stream == NULL) {
        appendAssistantStreamText((STRPTR)chunk, receivedMessage, wordNumber,
                                  speechUtf8Index);
        return;
    }

    if (!utf8stream_append(stream, (const UBYTE *)chunk, strlen(chunk))) {
        streamLogUtf8("append failed (buffer grow)");
        appendAssistantStreamText((STRPTR)chunk, receivedMessage, wordNumber,
                                  speechUtf8Index);
        return;
    }

    while ((taken = utf8stream_take_complete(stream, piece, sizeof(piece) - 1)) >
           0) {
        piece[taken] = '\0';
        appendAssistantStreamText((STRPTR)piece, receivedMessage, wordNumber,
                                  speechUtf8Index);
    }
}

static void flushUtf8StreamToMessage(struct UTF8StreamBuffer *stream,
                                     STRPTR receivedMessage, UWORD *wordNumber,
                                     ULONG *speechUtf8Index) {
    UBYTE piece[2048];
    ULONG taken;

    if (stream == NULL) {
        return;
    }

    taken = utf8stream_flush(stream, piece, sizeof(piece) - 1);
    if (taken > 0) {
        piece[taken] = '\0';
        appendAssistantStreamText((STRPTR)piece, receivedMessage, wordNumber,
                                  speechUtf8Index);
    }
}

/**
 * @brief Sends a chat message to the OpenAI API and displays the response
 *and speaks it if speech is enabled
 * @details This function sends a chat message to the OpenAI API and
 *displays the response in the chat window. It also speaks the response if
 *speech is enabled.
 **/
/* msgctxt "STRING_CHAT_RESPONSE_PARTIAL (275//)" */
/* msgid "Response incomplete (connection ended early)." */
/* msgctxt "STRING_CHAT_STREAM_TRUNCATED (276//)" */
/* msgid "Response truncated (stream buffer limit reached)." */

static ChatStreamOutcome chatStreamClassifyOutcome(UTF8 *receivedMessage) {
    struct ChatRequestSettings chatSettings;

    if (receivedMessage == NULL || receivedMessage[0] == '\0') {
        return CHAT_STREAM_FAILED;
    }
    configGetActiveChatRequestSettings(&chatSettings);
    if (!chatSettings.stream ||
        chatSettings.apiEndpoint != API_CHAT_ENDPOINT_RESPONSES) {
        return CHAT_STREAM_OK;
    }
    switch (openAIChatStreamTransportOutcome()) {
    case CHAT_TRANSPORT_OK:
        return CHAT_STREAM_OK;
    case CHAT_TRANSPORT_PARTIAL:
        return CHAT_STREAM_PARTIAL;
    case CHAT_TRANSPORT_FAILED:
        return CHAT_STREAM_FAILED;
    default:
        /* Continuation read: outcome set at end of each batch */
        if (openAIChatStreamCompletedOk()) {
            return CHAT_STREAM_OK;
        }
        return CHAT_STREAM_PARTIAL;
    }
}

static CONST_STRPTR chatStreamOutcomeName(ChatStreamOutcome outcome) {
    switch (outcome) {
    case CHAT_STREAM_OK:
        return "OK";
    case CHAT_STREAM_PARTIAL:
        return "PARTIAL";
    case CHAT_STREAM_FAILED:
    default:
        return "FAILED";
    }
}


#ifdef __MORPHOS__
static void setConversationListInputEnabled(BOOL enabled) {
    if (conversationListObject != NULL) {
        set(conversationListObject, MUIA_Disabled, !enabled);
    }
    if (conversationListViewObject != NULL) {
        set(conversationListViewObject, MUIA_Disabled, !enabled);
    }
}

static void conversationListReselectCurrent(void) {
    LONG total = 0;
    LONG i;

    if (currentConversation == NULL || conversationListObject == NULL) {
        return;
    }
    get(conversationListObject, MUIA_NList_Entries, &total);
    for (i = 0; i < total; i++) {
        struct Conversation *entry = NULL;
        DoMethod(conversationListObject, MUIM_NList_GetEntry, i, &entry);
        if (entry == currentConversation) {
            set(conversationListObject, MUIA_NList_Quiet, TRUE);
            DoMethod(conversationListObject, MUIM_NList_SetActive, i, NULL);
            set(conversationListObject, MUIA_NList_Quiet, FALSE);
            return;
        }
    }
}
#endif

static BOOL conversationListContains(struct Conversation *conversation) {
    LONG total = 0;
    LONG i;

    if (conversation == NULL || conversationListObject == NULL) {
        return FALSE;
    }
    get(conversationListObject, MUIA_NList_Entries, &total);
    for (i = 0; i < total; i++) {
        struct Conversation *entry = NULL;
        DoMethod(conversationListObject, MUIM_NList_GetEntry, i, &entry);
        if (entry == conversation) {
            return TRUE;
        }
    }
    return FALSE;
}

static void finishChatStream(ChatStreamOutcome outcome, UTF8 *receivedMessage,
                             ULONG speechUtf8Index, BOOL isNewConversation,
                             struct MinList *receivedFiles, BOOL requestStream,
                             struct Conversation *streamConversation) {
    struct ChatRequestSettings chatSettings;

#ifdef __MORPHOS__
    /* End live skip; allow Midi-Markdown on the one final paint (raw only
     * mid-stream ? forced raw here left periodensystem tables unformatted). */
    morphosChatLiveScintillaUpdates = FALSE;
    morphosChatStreamRawScintillaRefresh = FALSE;
#endif
    if (streamConversation == NULL) {
        streamConversation = currentConversation;
    }
    if (mainWindowIsShuttingDown()) {
        chatSendInProgress = FALSE;
#ifdef __MORPHOS__
        morphosSchedulePendingConversationSelect();
#endif
        return;
    }
    if (streamLogIsEnabled()) {
        ULONG msgLen =
            receivedMessage != NULL ? (ULONG)strlen(receivedMessage) : 0;
        streamLogChatEnd(chatStreamOutcomeName(outcome), msgLen,
                         openAIChatStreamCompletedOk(),
                         openAIChatStreamTruncated(),
                         openAIChatStreamLastSseSnippet());
    }

    configGetActiveChatRequestSettings(&chatSettings);

    /* Resolve every unanswered function_call before the next user message. */
    while (requestStream && hasPendingToolCall()) {
        UTF8 *responseId = getPendingResponseId();
        UWORD pendingCount = getPendingToolCallCount();
        STRPTR *callIds = NULL;
        STRPTR *outputs = NULL;
        UWORD i;
        BOOL userDenied = FALSE;
        BOOL denyRest = FALSE;
        BOOL allocFailed = FALSE;
        struct json_object *toolResponse;
        struct json_object *error;

        if (responseId == NULL || strlen(responseId) == 0 || pendingCount == 0) {
            clearPendingToolCall();
            break;
        }

        callIds = AllocVec(pendingCount * sizeof(STRPTR), MEMF_ANY | MEMF_CLEAR);
        outputs = AllocVec(pendingCount * sizeof(STRPTR), MEMF_ANY | MEMF_CLEAR);
        if (callIds == NULL || outputs == NULL) {
            if (callIds != NULL)
                FreeVec(callIds);
            if (outputs != NULL)
                FreeVec(outputs);
            clearPendingToolCall();
            break;
        }

        for (i = 0; i < pendingCount; i++) {
            STRPTR callId = NULL;
            STRPTR command = NULL;
            getPendingToolCallAt(i, &callId, NULL, &command);
            callIds[i] = callId;

            if (denyRest) {
                ULONG len = strlen(TOOL_CALL_OUTPUT_DENIED);
                outputs[i] = AllocVec(len + 1, MEMF_ANY | MEMF_CLEAR);
                if (outputs[i] != NULL)
                    strncpy(outputs[i], TOOL_CALL_OUTPUT_DENIED, len);
            } else if (chatSettings.shellToolEnabled && command != NULL &&
                       strlen(command) > 0) {
                UBYTE confirmMsg[4096];
                LONG result;
                snprintf(confirmMsg, sizeof(confirmMsg),
                         STRING_SHELL_TOOL_CONFIRMATION_BODY, command);
                result = MUI_Request(app, mainWindowObject,
#ifdef __MORPHOS__
                                     NULL,
#else
                                     MUIV_Requester_Image_Warning,
#endif
                                     STRING_SHELL_TOOL_CONFIRMATION_TITLE,
                                     STRING_SHELL_TOOL_CONFIRMATION_BUTTONS,
                                     confirmMsg, TAG_DONE);
                if (result != 1) {
                    ULONG len = strlen(TOOL_CALL_OUTPUT_DENIED);
                    outputs[i] = AllocVec(len + 1, MEMF_ANY | MEMF_CLEAR);
                    if (outputs[i] != NULL)
                        strncpy(outputs[i], TOOL_CALL_OUTPUT_DENIED, len);
                    userDenied = TRUE;
                    denyRest = TRUE;
                    strncat(chatOutputTextEditorContents,
                            STRING_SHELL_TOOL_DENIED_BANNER,
                            chatOutputTextEditorContentsCapacity -
                                strlen(chatOutputTextEditorContents) - 1);
                    strncat(receivedMessage, STRING_SHELL_TOOL_DENIED_BANNER,
                            READ_BUFFER_LENGTH - strlen(receivedMessage) - 1);
#ifndef __MORPHOS__
                    set(chatOutputTextEditor, MUIA_NFloattext_Text,
                        chatOutputTextEditorContents);
                    set(chatOutputListView, MUIA_NList_First,
                        MUIV_NList_First_Bottom);
#else
                    streamUiFlushChatDisplay();
#endif
                } else {
                    LONG exitCode = 0;
                    STRPTR output;
                    UBYTE statusMsg[256];
                    UBYTE cmdDisplay[512];
                    UBYTE outputDisplay[4096];
                    UBYTE toolOutput[8192];
                    ULONG len;

                    snprintf(statusMsg, sizeof(statusMsg),
                             STRING_EXECUTING_COMMAND);
                    updateStatusBar(statusMsg, yellowPen);
                    snprintf(cmdDisplay, sizeof(cmdDisplay),
                             STRING_SHELL_TOOL_EXECUTING_BANNER_FORMAT, command);
                    strncat(chatOutputTextEditorContents, cmdDisplay,
                            chatOutputTextEditorContentsCapacity -
                                strlen(chatOutputTextEditorContents) - 1);
                    strncat(receivedMessage, cmdDisplay,
                            READ_BUFFER_LENGTH - strlen(receivedMessage) - 1);
#ifndef __MORPHOS__
                    set(chatOutputTextEditor, MUIA_NFloattext_Text,
                        chatOutputTextEditorContents);
                    set(chatOutputListView, MUIA_NList_First,
                        MUIV_NList_First_Bottom);
#else
                    streamUiFlushChatDisplay();
#endif

                    output = executeShellCommand(command, &exitCode);
                    snprintf(outputDisplay, sizeof(outputDisplay),
                             STRING_SHELL_TOOL_OUTPUT_DISPLAY_FORMAT, exitCode,
                             output != NULL
                                 ? output
                                 : (STRPTR)STRING_SHELL_TOOL_NO_OUTPUT);
                    strncat(chatOutputTextEditorContents, outputDisplay,
                            chatOutputTextEditorContentsCapacity -
                                strlen(chatOutputTextEditorContents) - 1);
                    strncat(receivedMessage, outputDisplay,
                            READ_BUFFER_LENGTH - strlen(receivedMessage) - 1);
#ifndef __MORPHOS__
                    set(chatOutputTextEditor, MUIA_NFloattext_Text,
                        chatOutputTextEditorContents);
                    set(chatOutputListView, MUIA_NList_First,
                        MUIV_NList_First_Bottom);
#else
                    streamUiFlushChatDisplay();
#endif

                    snprintf(toolOutput, sizeof(toolOutput),
                             STRING_SHELL_TOOL_TOOL_OUTPUT_FORMAT, exitCode,
                             output != NULL
                                 ? output
                                 : (STRPTR)STRING_SHELL_TOOL_NO_OUTPUT);
                    len = strlen(toolOutput);
                    outputs[i] = AllocVec(len + 1, MEMF_ANY | MEMF_CLEAR);
                    if (outputs[i] != NULL)
                        strncpy(outputs[i], toolOutput, len);
                    if (output != NULL)
                        FreeVec(output);
                }
            } else {
                ULONG len = strlen(TOOL_CALL_OUTPUT_UNAVAILABLE);
                outputs[i] = AllocVec(len + 1, MEMF_ANY | MEMF_CLEAR);
                if (outputs[i] != NULL)
                    strncpy(outputs[i], TOOL_CALL_OUTPUT_UNAVAILABLE, len);
            }
            if (outputs[i] == NULL)
                allocFailed = TRUE;
        }

        if (allocFailed) {
            for (i = 0; i < pendingCount; i++) {
                if (outputs[i] != NULL)
                    FreeVec(outputs[i]);
            }
            FreeVec(callIds);
            FreeVec(outputs);
            clearPendingToolCall();
            break;
        }

        toolResponse = postToolResultsToOpenAI(
            responseId, (CONST_STRPTR *)callIds, (CONST_STRPTR *)outputs,
            pendingCount, chatSettings.model, chatSettings.host,
            chatSettings.port, chatSettings.useSSL, chatSettings.apiKey,
            configGetProxyEnabled(), chatSettings.proxyHost,
            chatSettings.proxyPort, chatSettings.proxyUsesSSL,
            chatSettings.proxyRequiresAuth, chatSettings.proxyUsername,
            chatSettings.proxyPassword, chatSettings.shellToolEnabled,
            chatSettings.codeInterpreterEnabled, chatSettings.apiEndpointUrl,
            chatSettings.authorizationType, chatSettings.customHeaders);

        for (i = 0; i < pendingCount; i++) {
            if (outputs[i] != NULL)
                FreeVec(outputs[i]);
        }
        FreeVec(callIds);
        FreeVec(outputs);

        if (toolResponse != NULL) {
            collectResponseFiles(toolResponse, receivedFiles);
            if (json_object_object_get_ex(toolResponse, "error", &error) &&
                !json_object_is_type(error, json_type_null)) {
                clearPendingToolCall();
                struct json_object *message =
                    json_object_object_get(error, "message");
                if (message != NULL) {
                    SetIoErr(0);
                    displayError(json_object_get_string(message));
                }
                json_object_put(toolResponse);
                break;
            }

            conversationSyncLastResponseIdFromPayload(streamConversation,
                                                      toolResponse);

            if (hasPendingToolCall()) {
                json_object_put(toolResponse);
                continue;
            }

            if (!userDenied) {
                UTF8 *toolContentString = getMessageContentFromJson(
                    toolResponse, FALSE, FALSE, chatSettings.apiEndpoint);
                if (toolContentString != NULL && strlen(toolContentString) > 0) {
                    strncat(receivedMessage, toolContentString,
                            READ_BUFFER_LENGTH - strlen(receivedMessage) - 1);

                    STRPTR formattedToolResponse = CodesetsUTF8ToStr(
                        CSA_DestCodeset, (Tag)systemCodeset, CSA_Source,
                        (Tag)toolContentString, CSA_MapForeignChars, TRUE,
                        TAG_DONE);
                    if (formattedToolResponse != NULL) {
                        strncat(chatOutputTextEditorContents,
                                formattedToolResponse,
                                chatOutputTextEditorContentsCapacity -
                                    strlen(chatOutputTextEditorContents) - 1);
                        CodesetsFreeA(formattedToolResponse, NULL);
                    } else {
                        STRPTR latin1 = utf8ToLatin1(toolContentString);
                        if (latin1 != NULL) {
                            strncat(chatOutputTextEditorContents, latin1,
                                    chatOutputTextEditorContentsCapacity -
                                        strlen(chatOutputTextEditorContents) - 1);
                            FreeVec(latin1);
                        }
                    }

#ifndef __MORPHOS__
                    STRPTR formattedContent = convertMarkdownFormattingToMUI(
                        chatOutputTextEditorContents);
                    if (formattedContent != NULL) {
                        strncpy(chatOutputTextEditorContents, formattedContent,
                                chatOutputTextEditorContentsCapacity - 1);
                        FreeVec(formattedContent);
                    }
                    set(chatOutputTextEditor, MUIA_NFloattext_Text,
                        chatOutputTextEditorContents);
                    set(chatOutputListView, MUIA_NList_First,
                        MUIV_NList_First_Bottom);
#else
                    streamUiFlushChatDisplay();
#endif
                }
            }
            json_object_put(toolResponse);
        } else {
            clearPendingToolCall();
            break;
        }
    } /* end of while (tool calls) */

    hideLoadingBar();

    if (outcome == CHAT_STREAM_OK || outcome == CHAT_STREAM_PARTIAL) {
#ifndef __MORPHOS__
        set(chatOutputTextEditor, MUIA_NFloattext_Text,
            chatOutputTextEditorContents);
#endif
        struct ConversationNode *assistantMessage = addTextToConversation(
            streamConversation, receivedMessage, "assistant");
        if (assistantMessage != NULL)
            moveChatFiles(&assistantMessage->files, receivedFiles);
        else
            freeChatFiles(receivedFiles);
        freeChatFiles(&pendingChatFiles);
        updateAttachmentControls();
        updateResponseFileControl();
#ifdef __MORPHOS__
        if (conversationRowPending == NULL ||
            conversationRowPending == streamConversation) {
            displayConversation(streamConversation);
            refreshViewCodeBlocksMenuState();
        } else {
            streamLogLifecycle(
                "stream display skipped; pending other conversation");
        }
#else
        displayConversation(streamConversation);
#endif

        if (configGetSpeechEnabled()) {
            SpeechSystem speechSys = configGetSpeechSystem();
            /* CJK + network TTS blocks the UI on the shared SSL socket for a
             * long time (looks like a hang after "chinesische Schriftzeichen"). */
            if (utf8_contains_cjk((const UBYTE *)receivedMessage)) {
                updateStatusBar(STRING_READY, greenPen);
            } else if (speechSystemUsesNetwork(speechSys)) {
                AudioFormat pcmFmt = AUDIO_FORMAT_PCM;
                AudioFormat *fmt =
                    (speechSys == SPEECH_SYSTEM_OPENAI ||
                     speechSys == SPEECH_SYSTEM_XAI)
                        ? &pcmFmt
                        : NULL;
                speakText(receivedMessage, NULL, fmt);
            } else {
                speakStreamUtf8Tail(receivedMessage, &speechUtf8Index);
            }
        }

        if (isNewConversation) {
            struct json_object **responses;
#ifdef __MORPHOS__
            if (conversationRowPending != NULL &&
                conversationRowPending != streamConversation) {
                /* User already switched away; avoid nested SSL/title work that
                 * freezes the app after a large stream. */
                if (streamConversation->name == NULL) {
                    streamConversation->name = allocNewConversationTitle(
                        streamConversation, NULL);
                    if (streamConversation->name != NULL) {
                        conversationRefreshNameListDisplay(streamConversation);
                    }
                }
                if (!conversationListContains(streamConversation)) {
                    DoMethod(conversationListObject, MUIM_NList_InsertSingle,
                             streamConversation, MUIV_NList_Insert_Top);
                }
                streamLogLifecycle(
                    "title gen skipped; pending other conversation");
                goto morphos_after_title_gen;
            }
#endif

            updateStatusBar(STRING_GENERATING_CONVERSATION_TITLE, 7);
            showLoadingBar();
            addTextToConversation(streamConversation,
                                  "generate a short title for this "
                                  "conversation and don't enclose the title in "
                                  "quotes or prefix the response with anything",
                                  "user");
            setConversationSystem(streamConversation, NULL);
            responses = postChatMessageToOpenAI(
                streamConversation, chatSettings.host, chatSettings.port,
                chatSettings.useSSL, chatSettings.model, chatSettings.apiKey,
                FALSE, chatSettings.useProxy, chatSettings.proxyHost,
                chatSettings.proxyPort, chatSettings.proxyUsesSSL,
                chatSettings.proxyRequiresAuth, chatSettings.proxyUsername,
                chatSettings.proxyPassword, FALSE, FALSE, FALSE,
                chatSettings.apiEndpoint, chatSettings.apiEndpointUrl,
                chatSettings.authorizationType, chatSettings.customHeaders);
            struct Node *titleRequestNode =
                RemTail((struct List *)streamConversation->messages);
            freeConversationNode(
                (struct ConversationNode *)titleRequestNode);
            hideLoadingBar();
            if (responses == NULL) {
                if (!isRequestCancelled())
                    displayError(STRING_ERROR_CONNECTING_OPENAI);
            } else if (responses[0] != NULL) {
                /* Concatenate all text parts across response objects */
                ULONG combinedLen = 1;
                UWORD ri = 0;
                struct json_object *r = NULL;
                STRPTR combined = NULL;

                while ((r = responses[ri++]) != NULL) {
                    UTF8 *part = getMessageContentFromJson(
                        r, FALSE, FALSE, chatSettings.apiEndpoint);
                    if (part != NULL)
                        combinedLen += strlen(part) + 1;
                }
                combined = AllocVec(combinedLen, MEMF_ANY | MEMF_CLEAR);
                if (combined == NULL) {
                    combined = (STRPTR) "";
                } else {
                    ri = 0;
                    while ((r = responses[ri++]) != NULL) {
                        UTF8 *part = getMessageContentFromJson(
                            r, FALSE, FALSE, chatSettings.apiEndpoint);
                        if (part != NULL && strlen(part) > 0) {
                            strncat(combined, part,
                                    combinedLen - strlen(combined) - 1);
                        }
                    }
                }
                if (streamConversation->name == NULL) {
                    streamConversation->name = allocNewConversationTitle(
                        streamConversation, (const UTF8 *)combined);
                    if (streamConversation->name != NULL) {
                        conversationRefreshNameListDisplay(streamConversation);
                    }
                }
                if (combined != NULL && combined != (STRPTR) "") {
                    FreeVec(combined);
                }
                if (!conversationListContains(streamConversation)) {
                    DoMethod(conversationListObject, MUIM_NList_InsertSingle,
                             streamConversation, MUIV_NList_Insert_Top);
                }
#ifdef __MORPHOS__
                if (conversationRowPending == NULL ||
                    conversationRowPending == streamConversation) {
                    DoMethod(conversationListObject, MUIM_NList_SetActive,
                             MUIV_NList_Active_Top, NULL);
                    currentConversation = streamConversation;
                } else {
                    /* Keep user's NList selection; apply after send. */
                    currentConversation = streamConversation;
                    streamLogLifecycle(
                        "new chat inserted; keep pending list selection");
                }
#else
                DoMethod(conversationListObject, MUIM_NList_SetActive,
                         MUIV_NList_Active_Top, NULL);
                currentConversation = streamConversation;
#endif
                ri = 0;
                r = NULL;
                while ((r = responses[ri++]) != NULL) {
                    json_object_put(r);
                }
            }
            FreeVec(responses);
        }
#ifdef __MORPHOS__
    morphos_after_title_gen:;
#endif
    }

    if (outcome == CHAT_STREAM_PARTIAL) {
        if (openAIChatStreamTruncated()) {
            updateStatusBar(STRING_CHAT_STREAM_TRUNCATED, yellowPen);
        } else {
            updateStatusBar(STRING_CHAT_RESPONSE_PARTIAL, yellowPen);
        }
    }

    if (!mainWindowIsShuttingDown()) {
        saveConversations();
    }

    finishActiveRequest(outcome != CHAT_STREAM_PARTIAL);
    chatSendInProgress = FALSE;
#ifdef __MORPHOS__
    morphosSchedulePendingConversationSelect();
#endif
}

static void sendChatMessage() {
    BOOL isNewConversation = FALSE;
    struct json_object **responses;
    struct MinList receivedFiles;
    BOOL requestStream;
    struct ConversationNode *userMessage = NULL;

    streamLogLifecycle("sendChatMessage begin");
    chatSendInProgress = TRUE;
    if (currentConversation == NULL) {
        isNewConversation = TRUE;
        currentConversation = newConversation();
#ifdef __MORPHOS__
        clearChatOutputDisplay();
#else
        chatOutputTextEditorContents[0] = '\0';
        set(chatOutputTextEditor, MUIA_NFloattext_Text,
            chatOutputTextEditorContents);
#endif
        set(conversationListObject, MUIA_NList_Active, MUIV_NList_Active_Off);
    }

    updateStatusBar(STRING_SENDING_MESSAGE, yellowPen);
    showLoadingBar();
    UTF8 *receivedMessage = AllocVec(READ_BUFFER_LENGTH, MEMF_ANY | MEMF_CLEAR);
    NewList((struct List *)&receivedFiles);
    STRPTR text;
    if (isAROS) {
        get(chatInputTextEditor, MUIA_String_Contents, &text);
    } else {
        text = DoMethod(chatInputTextEditor, MUIM_TextEditor_ExportText);
    }
    BOOL freeExportedText = !isAROS && text != NULL;
    if (text == NULL)
        text = (STRPTR)"";

    while (strlen(text) > 0 && text[strlen(text) - 1] == '\n')
        text[strlen(text) - 1] = '\0';
    if (strlen(text) == 0 && chatFileCount(&pendingChatFiles) == 0) {
        displayError(STRING_ERROR_MESSAGE_OR_ATTACHMENT_REQUIRED);
        hideLoadingBar();
        FreeVec(receivedMessage);
        if (isNewConversation) {
            freeConversation(currentConversation);
            currentConversation = NULL;
        }
        if (freeExportedText)
            FreeVec(text);
        chatSendInProgress = FALSE;
#ifdef __MORPHOS__
        morphosSchedulePendingConversationSelect();
#endif
        return;
    }

    beginCancellableRequest();
    setRequestInterfaceBusy(TRUE);

    UTF8 *textUTF8 = CodesetsUTF8Create(CSA_SourceCodeset, (Tag)systemCodeset,
                                        CSA_Source, (Tag)text,
                                        CSA_MapForeignChars, TRUE, TAG_DONE);

#ifndef __MORPHOS__
    {
        UBYTE userStyleString[] = "\033r\033b\0333";
        UBYTE userAlignment = 'l';
        switch (configGetUserTextAlignment()) {
        case ALIGN_LEFT:
            userAlignment = 'l';
            break;
        case ALIGN_CENTER:
            userAlignment = 'c';
            break;
        case ALIGN_RIGHT:
            userAlignment = 'r';
            break;
        }
        userStyleString[1] = userAlignment;
        strbufAppend(chatOutputTextEditorContents,
                     CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH, userStyleString);
        for (ULONG i = 0; i < strlen(text); i++) {
            UBYTE oneChar[2] = {0, 0};

            if (strlen(chatOutputTextEditorContents) >=
                CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH - 10) {
                break;
            }
            oneChar[0] = text[i];
            strbufAppend(chatOutputTextEditorContents,
                         CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH,
                         (STRPTR)oneChar);
            if (text[i] == '\n') {
                strbufAppend(chatOutputTextEditorContents,
                             CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH,
                             userStyleString);
            }
        }

        set(chatOutputTextEditor, MUIA_NFloattext_Text,
            chatOutputTextEditorContents);
        set(chatOutputListView, MUIA_NList_First, MUIV_NList_First_Bottom);
    }
#endif

    userMessage = addTextToConversation(
        currentConversation, textUTF8 != NULL ? textUTF8 : (UTF8 *)"", "user");
    if (textUTF8 != NULL)
        CodesetsFreeA(textUTF8, NULL);
    if (userMessage == NULL ||
        !copyChatFiles(&userMessage->files, &pendingChatFiles)) {
        displayError(STRING_ERROR_MEMORY_CONVERSATION_NODE);
        finishActiveRequest(FALSE);
        if (userMessage != NULL) {
            RemTail((struct List *)currentConversation->messages);
            freeConversationNode(userMessage);
        }
        if (isNewConversation) {
            freeConversation(currentConversation);
            currentConversation = NULL;
#ifdef __MORPHOS__
            clearChatOutputDisplay();
#else
            chatOutputTextEditorContents[0] = '\0';
            set(chatOutputTextEditor, MUIA_NFloattext_Text,
                chatOutputTextEditorContents);
#endif
        }
        FreeVec(receivedMessage);
        if (freeExportedText)
            FreeVec(text);
        chatSendInProgress = FALSE;
#ifdef __MORPHOS__
        morphosSchedulePendingConversationSelect();
#endif
        return;
    }

    struct ChatRequestSettings chatSettings;
    configGetActiveChatRequestSettings(&chatSettings);

    BOOL xaiHost = chatSettings.host != NULL &&
                   strcmp(chatSettings.host, "api.x.ai") == 0;
    BOOL cloudAttachmentHost =
        (chatSettings.host != NULL &&
         (hostIsOpenAICompatible(chatSettings.host,
                                 chatSettings.apiEndpointUrl) ||
          strcmp(chatSettings.host, "generativelanguage.googleapis.com") ==
              0 ||
          strcmp(chatSettings.host, "api.anthropic.com") == 0));
    requestStream = chatSettings.stream;
    if (chatFileCount(&userMessage->files) > 0) {
        if (xaiHost)
            requestStream = TRUE;
        else if (cloudAttachmentHost)
            requestStream = FALSE;
    }

    setConversationSystem(currentConversation, chatSettings.chatSystem);

    if (isAROS) {
        set(chatInputTextEditor, MUIA_String_Contents, "");
    } else {
        DoMethod(chatInputTextEditor, MUIM_TextEditor_ClearText);
    }
    DoMethod(chatInputTextEditor, MUIM_GoActive);

    BOOL dataStreamFinished = FALSE;
    ULONG speechUtf8Index = 0;
    UWORD wordNumber = 0;
    struct UTF8StreamBuffer *utf8Stream = utf8stream_create(4096);

    streamUiResetRefreshClock();
#ifdef __MORPHOS__
    /* Arm live-skip before any buffer rebuild so a deferred Scintilla refresh
     * cannot docswap+APPENDTEXT while SSE is about to start (OS freeze). */
    morphosChatStreamRawScintillaRefresh = TRUE;
    morphosChatLiveScintillaUpdates = TRUE;
    displayConversation(currentConversation);
#endif

    strbufAppend(chatOutputTextEditorContents,
                 CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH, "\n");

    do {
        responses = postChatMessageToOpenAI(
            currentConversation, chatSettings.host, chatSettings.port,
            chatSettings.useSSL, chatSettings.model, chatSettings.apiKey,
            requestStream, chatSettings.useProxy, chatSettings.proxyHost,
            chatSettings.proxyPort, chatSettings.proxyUsesSSL,
            chatSettings.proxyRequiresAuth, chatSettings.proxyUsername,
            chatSettings.proxyPassword, chatSettings.webSearchEnabled,
            chatSettings.shellToolEnabled, chatSettings.codeInterpreterEnabled,
            chatSettings.apiEndpoint, chatSettings.apiEndpointUrl,
            chatSettings.authorizationType, chatSettings.customHeaders);
        if (responses == NULL) {
            streamLogLifecycle("chat send postChat returned null");
            streamLogApiError("connect", "postChatMessageToOpenAI returned NULL");
            if (!isRequestCancelled())
                displayError(STRING_ERROR_CONNECTING_OPENAI);
            finishActiveRequest(FALSE);
            struct ConversationNode *failedMessage =
                (struct ConversationNode *)RemTail(
                    (struct List *)currentConversation->messages);
            freeConversationNode(failedMessage);
            if (isNewConversation) {
                freeConversation(currentConversation);
                currentConversation = NULL;
#ifdef __MORPHOS__
                clearChatOutputDisplay();
#else
                chatOutputTextEditorContents[0] = '\0';
                set(chatOutputTextEditor, MUIA_NFloattext_Text,
                    chatOutputTextEditorContents);
#endif
            }
            if (isAROS) {
                set(chatInputTextEditor, MUIA_String_Contents, text);
            } else {
                set(chatInputTextEditor, MUIA_TextEditor_Contents, text);
            }
            freeChatFiles(&receivedFiles);
            utf8stream_free(utf8Stream);
            FreeVec(receivedMessage);
            if (freeExportedText)
                FreeVec(text);
#ifdef __MORPHOS__
            morphosChatStreamRawScintillaRefresh = FALSE;
            morphosChatLiveScintillaUpdates = FALSE;
#endif
            chatSendInProgress = FALSE;
#ifdef __MORPHOS__
            morphosSchedulePendingConversationSelect();
#endif
            return;
        }


        UWORD responseIndex = 0;
        struct json_object *response;
        if (mainWindowIsShuttingDown()) {
            freeOpenAIResponseArray(responses);
            responses = NULL;
            dataStreamFinished = TRUE;
            break;
        }

        while (response = responses[responseIndex++]) {
            collectResponseFiles(response, &receivedFiles);
            struct json_object *error;
            if (json_object_object_get_ex(response, "error", &error) &&
                !json_object_is_type(error, json_type_null)) {
                CONST_STRPTR messageString = jsonGetApiErrorMessage(error);
                streamLogApiError("api_json_error", messageString);
                STRPTR formattedMessageSystemEncoded = CodesetsUTF8ToStr(
                    CSA_DestCodeset, (Tag)systemCodeset, CSA_Source,
                    (Tag)(messageString != NULL ? messageString : ""),
                    CSA_MapForeignChars, TRUE, TAG_DONE);
                displayError(formattedMessageSystemEncoded);
                CodesetsFreeA(formattedMessageSystemEncoded, NULL);
                if (isAROS) {
                    set(chatInputTextEditor, MUIA_String_Contents, text);
                } else {
                    set(chatInputTextEditor, MUIA_TextEditor_Contents, text);
                }
                struct Node *lastMessage =
                    RemTail((struct List *)currentConversation->messages);
                freeConversationNode((struct ConversationNode *)lastMessage);
                if (isNewConversation &&
                    currentConversation->messages->mlh_Head->mln_Succ ==
                        NULL) {
                    freeConversation(currentConversation);
                    currentConversation = NULL;
#ifdef __MORPHOS__
                    clearChatOutputDisplay();
#else
                    chatOutputTextEditorContents[0] = '\0';
                    set(chatOutputTextEditor, MUIA_NFloattext_Text,
                        chatOutputTextEditorContents);
#endif
                } else {
                    displayConversation(currentConversation);
#ifdef __MORPHOS__
                    refreshViewCodeBlocksMenuState();
#endif
                }
                json_object_put(response);
                discardOpenAIResponseArray(responses, responseIndex);
                responses = NULL;

                finishActiveRequest(FALSE);

                freeChatFiles(&receivedFiles);
                utf8stream_free(utf8Stream);
                FreeVec(receivedMessage);
                if (freeExportedText)
                    FreeVec(text);
#ifdef __MORPHOS__
                morphosChatStreamRawScintillaRefresh = FALSE;
                morphosChatLiveScintillaUpdates = FALSE;
#endif
                chatSendInProgress = FALSE;
#ifdef __MORPHOS__
                morphosSchedulePendingConversationSelect();
#endif
                return;
            }

            if (!json_object_is_type(response, json_type_object)) {
                json_object_put(response);
                continue;
            }

            UTF8 *contentString = getMessageContentFromJson(
                response, requestStream, FALSE, chatSettings.apiEndpoint);
            if (!requestStream) {
                appendValidatedUtf8Chunk(utf8Stream, contentString,
                                         receivedMessage, &wordNumber,
                                         &speechUtf8Index);
                json_object_put(response);
                dataStreamFinished = TRUE;
                continue;
            } else {
                if (contentString != NULL) {
                    if (strlen(contentString) > 0) {
                        appendValidatedUtf8Chunk(utf8Stream, contentString,
                                                 receivedMessage, &wordNumber,
                                                 &speechUtf8Index);
                    }
                    struct json_object *typeObj = NULL;
                    STRPTR type = NULL;

                    if (json_object_object_get_ex(response, "type", &typeObj)) {
                        type = json_object_get_string(typeObj);
                    }
                    if (type != NULL &&
                        strcmp(type, "response.output_text.done") == 0 &&
                        chatSettings.apiEndpoint ==
                            API_CHAT_ENDPOINT_RESPONSES) {
                        /* Authoritative full segment text; deltas can be a
                         * short prefix when tools (web_search) intervene. */
                        struct json_object *doneTextObj =
                            json_object_object_get(response, "text");
                        UTF8 *doneText =
                            doneTextObj != NULL
                                ? (UTF8 *)json_object_get_string(doneTextObj)
                                : NULL;
                        {
                            ULONG doneLen =
                                doneText != NULL
                                    ? (ULONG)strlen((char *)doneText)
                                    : 0;
                            ULONG accLen =
                                (ULONG)strlen((char *)receivedMessage);
                            if (streamLogIsEnabled()) {
                                UBYTE detail[160];
                                snprintf((char *)detail, sizeof(detail),
                                         "doneMsg=%lu accumulated=%lu "
                                         "supersede=%u",
                                         (unsigned long)doneLen,
                                         (unsigned long)accLen,
                                         (unsigned)(doneLen > accLen));
                                streamLogApiError("stream_output_text_done",
                                                  (CONST_STRPTR)detail);
                            }
                            if (doneText != NULL && doneLen > accLen) {
                                ULONG n = doneLen;
                                if (n >= READ_BUFFER_LENGTH)
                                    n = READ_BUFFER_LENGTH - 1;
                                CopyMem(doneText, receivedMessage, n);
                                receivedMessage[n] = '\0';
                                speechUtf8Index = 0;
                                wordNumber = 0;
                                utf8stream_free(utf8Stream);
                                utf8Stream = utf8stream_create(4096);
                                streamLogLifecycle(
                                    "stream output_text.done supersedes "
                                    "deltas");
                            }
                        }
                    } else if (type != NULL &&
                               strcmp(type, "response.completed") == 0) {
                        if (chatSettings.apiEndpoint ==
                            API_CHAT_ENDPOINT_RESPONSES) {
                            struct json_object *nested =
                                json_object_object_get(response, "response");
                            UTF8 *finalText = getMessageContentFromJson(
                                nested != NULL ? nested : response, FALSE,
                                FALSE, API_CHAT_ENDPOINT_RESPONSES);
                            {
                                ULONG finalLen =
                                    finalText != NULL
                                        ? (ULONG)strlen((char *)finalText)
                                        : 0;
                                ULONG accLen =
                                    (ULONG)strlen((char *)receivedMessage);
                                if (streamLogIsEnabled()) {
                                    UBYTE detail[160];
                                    snprintf(
                                        (char *)detail, sizeof(detail),
                                        "completedMsg=%lu accumulated=%lu "
                                        "supersede=%u",
                                        (unsigned long)finalLen,
                                        (unsigned long)accLen,
                                        (unsigned)(finalLen > accLen));
                                    streamLogApiError("stream_completed",
                                                      (CONST_STRPTR)detail);
                                }
                                if (finalText != NULL && finalLen > accLen) {
                                    ULONG n = finalLen;
                                    if (n >= READ_BUFFER_LENGTH)
                                        n = READ_BUFFER_LENGTH - 1;
                                    CopyMem(finalText, receivedMessage, n);
                                    receivedMessage[n] = '\0';
                                    speechUtf8Index = 0;
                                    wordNumber = 0;
                                    utf8stream_free(utf8Stream);
                                    utf8Stream = utf8stream_create(4096);
                                    streamLogLifecycle(
                                        "stream response.completed supersedes "
                                        "partial deltas");
                                }
                            }
                        }
                        dataStreamFinished = TRUE;
                    } else if (json_object_object_get(response, "choices") !=
                               NULL) {
                        struct json_object *choices =
                            json_object_object_get(response, "choices");
                        if (choices != NULL &&
                            json_object_is_type(choices, json_type_array) &&
                            json_object_array_length(choices) > 0) {
                            struct json_object *choice0 =
                                json_object_array_get_idx(choices, 0);
                            if (choice0 != NULL) {
                                struct json_object *finishReason =
                                    json_object_object_get(choice0,
                                                           "finish_reason");
                                if (finishReason != NULL &&
                                    !json_object_is_type(finishReason,
                                                         json_type_null)) {
                                    UTF8 *fr =
                                        json_object_get_string(finishReason);
                                    if (fr != NULL && strlen(fr) > 0) {
                                        dataStreamFinished = TRUE;
                                    }
                                }
                            }
                        }
                    } else {
                        struct json_object *candidates =
                            json_object_object_get(response, "candidates");
                        if (candidates != NULL &&
                            json_object_is_type(candidates, json_type_array) &&
                            json_object_array_length(candidates) > 0) {
                            struct json_object *cand0 =
                                json_object_array_get_idx(candidates, 0);
                            if (cand0 != NULL &&
                                json_object_is_type(cand0, json_type_object)) {
                                struct json_object *finishReason =
                                    json_object_object_get(cand0,
                                                           "finishReason");
                                if (finishReason == NULL) {
                                    finishReason = json_object_object_get(
                                        cand0, "finish_reason");
                                }
                                if (finishReason != NULL &&
                                    !json_object_is_type(finishReason,
                                                         json_type_null)) {
                                    UTF8 *fr =
                                        json_object_get_string(finishReason);
                                    if (fr != NULL && strlen(fr) > 0) {
                                        dataStreamFinished = TRUE;
                                    }
                                }
                            }
                        }
                    }
                    json_object_put(response);
                } else {
                    dataStreamFinished = TRUE;
                }
            }
        }
        freeOpenAIResponseArray(responses);
        responses = NULL;

#ifndef DAEMON
        if (app != NULL) {
            ULONG muiSig = 0;
            DoMethod(app, MUIM_Application_NewInput, &muiSig);
        }
#endif
        if (requestStream &&
            chatSettings.apiEndpoint == API_CHAT_ENDPOINT_RESPONSES &&
            !openAIChatStreamInProgress()) {
            dataStreamFinished = TRUE;
        }
    } while (!dataStreamFinished && !mainWindowIsShuttingDown());

    flushUtf8StreamToMessage(utf8Stream, receivedMessage, &wordNumber,
                             &speechUtf8Index);
    utf8stream_free(utf8Stream);

    if (!mainWindowIsShuttingDown()) {
        streamUiFlushChatDisplay();
        finishChatStream(chatStreamClassifyOutcome(receivedMessage),
                         receivedMessage, speechUtf8Index, isNewConversation,
                         &receivedFiles, requestStream, currentConversation);
    } else {
        freeChatFiles(&receivedFiles);
        endCancellableRequest();
        chatSendInProgress = FALSE;
#ifdef __MORPHOS__
        morphosSchedulePendingConversationSelect();
#endif
    }

    streamLogLifecycle("sendChatMessage end");
    FreeVec(receivedMessage);
    if (freeExportedText)
        FreeVec(text);
}

static void appendMessageFileSummary(struct ConversationNode *message) {
    if (message == NULL || message->files.mlh_Head->mln_Succ == NULL)
        return;
    CONST_STRPTR prefix = strcmp(message->role, "assistant") == 0
                              ? STRING_RECEIVED_FILES_PREFIX
                              : STRING_ATTACHED_FILES_PREFIX;
    strncat(chatOutputTextEditorContents, "\n[",
            chatOutputTextEditorContentsCapacity -
                strlen(chatOutputTextEditorContents) - 1);
    strncat(chatOutputTextEditorContents, prefix,
            chatOutputTextEditorContentsCapacity -
                strlen(chatOutputTextEditorContents) - 1);
    struct ChatFile *file;
    ULONG index = 0;
    for (file = (struct ChatFile *)message->files.mlh_Head;
         file->node.mln_Succ != NULL;
         file = (struct ChatFile *)file->node.mln_Succ) {
        if (index++ > 0)
            strncat(chatOutputTextEditorContents, ", ",
                    chatOutputTextEditorContentsCapacity -
                        strlen(chatOutputTextEditorContents) - 1);
        STRPTR name = CodesetsUTF8ToStr(
            CSA_DestCodeset, (Tag)systemCodeset, CSA_Source, (Tag)file->name,
            CSA_MapForeignChars, TRUE, TAG_DONE);
        strncat(chatOutputTextEditorContents,
                name != NULL ? name : (STRPTR)file->name,
                chatOutputTextEditorContentsCapacity -
                    strlen(chatOutputTextEditorContents) - 1);
        if (name != NULL)
            CodesetsFreeA(name, NULL);
    }
    strncat(chatOutputTextEditorContents, "]",
            chatOutputTextEditorContentsCapacity -
                strlen(chatOutputTextEditorContents) - 1);
}


/**
 * Prints the conversation to the conversation window
 * @param conversation the conversation to display
 **/
void displayConversation(struct Conversation *conversation) {
    struct ConversationNode *conversationNode;
    UTF8 *content;

    if (mainWindowIsShuttingDown()) {
        return;
    }
#ifdef __MORPHOS__
    streamLogLifecycle("displayConversation build buffer begin");
#endif
    if (conversation == NULL) {
        conversation = currentConversation;
    }
    if (conversation == NULL || chatOutputTextEditorContents == NULL) {
        return;
    }

    chatOutputTextEditorContents[0] = '\0';

    ULONG estimatedRequired = 1024;
    for (conversationNode =
             (struct ConversationNode *)conversation->messages->mlh_Head;
         conversationNode->node.mln_Succ != NULL;
        conversationNode =
             (struct ConversationNode *)conversationNode->node.mln_Succ) {
        estimatedRequired +=
            strlen((const char *)(conversationNode->content != NULL
                                              ? conversationNode->content
                                              : (UTF8 *)"")) *
                3 +
            512;
        struct ChatFile *file;
        for (file = (struct ChatFile *)conversationNode->files.mlh_Head;
             file->node.mln_Succ != NULL;
             file = (struct ChatFile *)file->node.mln_Succ)
            estimatedRequired += strlen(file->name) * 3 + 8;
    }
    if (!ensureChatOutputBufferCapacity(estimatedRequired)) {
        displayError(STRING_ERROR_CONVERSATION_MAX_LENGTH_EXCEEDED);
        set(sendMessageButton, MUIA_Disabled, TRUE);
        return;
    }

    for (conversationNode =
             (struct ConversationNode *)conversation->messages->mlh_Head;
         conversationNode->node.mln_Succ != NULL;
         conversationNode =
             (struct ConversationNode *)conversationNode->node.mln_Succ) {
        content = conversationNodeGetDisplay(conversationNode);
        if (content == NULL) {
            content = (UTF8 *)"";
        }
        UTF8 *rawNode = conversationNodeGetRaw(conversationNode);
        if ((strlen(chatOutputTextEditorContents) +
             strlen((const char *)(rawNode != NULL ? rawNode : (UTF8 *)"")) +
             256) >
            chatOutputTextEditorContentsCapacity) {
            ULONG need = strlen(chatOutputTextEditorContents) +
                         strlen((const char *)(rawNode != NULL ? rawNode : (UTF8 *)"")) *
                             3 +
                         1024;
            if (!ensureChatOutputBufferCapacity(need)) {
                displayError(STRING_ERROR_CONVERSATION_MAX_LENGTH_EXCEEDED);
                set(sendMessageButton, MUIA_Disabled, TRUE);
                return;
            }
        }
        if ((strlen(chatOutputTextEditorContents) + strlen(content) + 256) >
            WRITE_BUFFER_LENGTH) {
            displayError(STRING_ERROR_CONVERSATION_MAX_LENGTH_EXCEEDED);
            break;
        }
#ifdef __MORPHOS__
        if (chatOutputTextEditorContents[0] != '\0') {
            strbufAppend(chatOutputTextEditorContents,
                         CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH, "\n\n");
        }
        strbufAppend(chatOutputTextEditorContents,
                     CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH, content);
#else
        if (strcmp(conversationNode->role, "user") == 0) {
            UTF8 *userRaw = conversationNodeGetRaw(conversationNode);
            STRPTR userContent =
                CodesetsUTF8ToStr(CSA_DestCodeset, (Tag)systemCodeset,
                                  CSA_Source, (Tag)userRaw,
                                  CSA_MapForeignChars, TRUE, TAG_DONE);
            STRPTR latin1Fallback = NULL;
            BOOL freeWithCodesets = (userContent != NULL);
            if (userContent == NULL) {
                latin1Fallback = utf8ToLatin1(userRaw);
                userContent = latin1Fallback ? latin1Fallback
                                             : (STRPTR)userRaw;
            }
            content = (UTF8 *)userContent;
            UBYTE userAlignment = 'l';
            switch (configGetUserTextAlignment()) {
            case ALIGN_LEFT:
                userAlignment = 'l';
                break;
            case ALIGN_CENTER:
                userAlignment = 'c';
                break;
            case ALIGN_RIGHT:
                userAlignment = 'r';
                break;
            }
            UBYTE userStyleString[] = "\033r\033b\0333";
            userStyleString[1] = userAlignment;
            strbufAppend(chatOutputTextEditorContents,
                         CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH,
                         userStyleString);
            for (ULONG i = 0; i < strlen(content); i++) {
                UBYTE oneChar[2] = {0, 0};

                oneChar[0] = content[i];
                strbufAppend(chatOutputTextEditorContents,
                             CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH,
                             (STRPTR)oneChar);
                if (content[i] == '\n') {
                    strbufAppend(chatOutputTextEditorContents,
                                 CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH,
                                 userStyleString);
                }
            }
            if (freeWithCodesets)
                CodesetsFreeA(userContent, NULL);
            else if (latin1Fallback != NULL)
                FreeVec(latin1Fallback);
            appendMessageFileSummary(conversationNode);
        } else if (strcmp(conversationNode->role, "assistant") == 0) {
            set(chatOutputListView, MUIA_NFloattext_Align,
                configGetAssistantTextAlignment());
            UBYTE assistantStyleString[] = "\n\n\0332";
            STRPTR formattedContent = formatAssistantTextForDisplay(content);

            strbufAppend(chatOutputTextEditorContents,
                         CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH,
                         assistantStyleString);
            if (formattedContent != NULL) {
                strbufAppend(chatOutputTextEditorContents,
                             CHAT_OUTPUT_TEXT_EDITOR_CONTENTS_LENGTH,
                             formattedContent);
                FreeVec(formattedContent);
            }
            appendMessageFileSummary(conversationNode);
            const UBYTE messageSeparatorStyleString[] = "\n\n";
            strncat(chatOutputTextEditorContents, messageSeparatorStyleString,
                    strlen(messageSeparatorStyleString));
        }
#endif
    }

#ifdef __MORPHOS__
    streamLogLifecycle("displayConversation build buffer done");
    morphosScheduleChatOutputRefreshFromList();
#else
    {
        ULONG totalLen = (ULONG)strlen(chatOutputTextEditorContents);
        if (totalLen > CHAT_OUTPUT_WIDGET_SAFE_LIMIT) {
            STRPTR truncated = chatOutputTextEditorContents +
                               (totalLen - CHAT_OUTPUT_WIDGET_SAFE_LIMIT);
            memmove(chatOutputTextEditorContents, truncated,
                    CHAT_OUTPUT_WIDGET_SAFE_LIMIT + 1);
        }

        set(chatOutputTextEditor, MUIA_NFloattext_Text,
            chatOutputTextEditorContents);
        set(chatOutputListView, MUIA_NList_First, MUIV_NList_First_Bottom);
    }
#endif
}

/**
 * Copy a conversation
 * @param conversation The conversation to copy
 * @return A pointer to the copied conversation
 **/
static struct Conversation *
copyConversation(struct Conversation *conversation) {
    struct Conversation *copy = newConversation();
    struct ConversationNode *conversationNode;
    for (conversationNode =
             (struct ConversationNode *)conversation->messages->mlh_Head;
         conversationNode->node.mln_Succ != NULL;
         conversationNode =
             (struct ConversationNode *)conversationNode->node.mln_Succ) {
        struct ConversationNode *copiedMessage = addTextToConversation(
            copy, conversationNodeGetRaw(conversationNode),
            conversationNode->role);
        if (copiedMessage != NULL)
            copyChatFiles(&copiedMessage->files, &conversationNode->files);
    }
    if (conversation->name != NULL) {
        copy->name = dupStringAlloc(conversation->name);
        if (copy->name != NULL) {
            conversationRefreshNameListDisplay(copy);
        }
    }
    if (conversation->lastResponseId != NULL &&
        strlen(conversation->lastResponseId) > 0) {
        copy->lastResponseId = AllocVec(
            strlen(conversation->lastResponseId) + 1, MEMF_ANY | MEMF_CLEAR);
        if (copy->lastResponseId != NULL) {
            strncpy(copy->lastResponseId, conversation->lastResponseId,
                    strlen(conversation->lastResponseId));
        }
    }
    return copy;
}

/**
 * Copy a generated image
 * @param generatedImage The generated image to copy
 * @return A pointer to the copied generated image
 **/
static struct GeneratedImage *
copyGeneratedImage(struct GeneratedImage *generatedImage) {
    struct GeneratedImage *newEntry =
        AllocVec(sizeof(struct GeneratedImage), MEMF_CLEAR);
    newEntry->name = dupStringAlloc(generatedImage->name);
    newEntry->filePath = dupStringAlloc(generatedImage->filePath);
    newEntry->prompt = dupStringAlloc(generatedImage->prompt);
    newEntry->imageModel = generatedImage->imageModel;
    newEntry->width = generatedImage->width;
    newEntry->height = generatedImage->height;
    return (newEntry);
}

/**
 * Saves the conversations to disk
 * @return RETURN_OK on success, RETURN_ERROR on failure
 **/
static LONG saveConversations() {
    BPTR file = Open("AMIGAGPT:chat-history.json", MODE_NEWFILE);
    if (file == 0) {
        displayError(STRING_ERROR_CHAT_HISTORY_CREATE);
        return RETURN_ERROR;
    }

    struct json_object *conversationsJsonArray = json_object_new_array();

    LONG totalConversationCount;
    get(conversationListObject, MUIA_NList_Entries, &totalConversationCount);

    for (LONG i = 0; i < totalConversationCount; i++) {
        struct json_object *conversationJsonObject = json_object_new_object();
        struct Conversation *conversation;
        DoMethod(conversationListObject, MUIM_NList_GetEntry, i, &conversation);
        json_object_object_add(conversationJsonObject, "name",
                               json_object_new_string(conversation->name));
        if (conversation->lastResponseId != NULL &&
            strlen(conversation->lastResponseId) > 0) {
            json_object_object_add(
                conversationJsonObject, "lastResponseId",
                json_object_new_string(conversation->lastResponseId));
        }
        struct json_object *messagesJsonArray = json_object_new_array();
        struct ConversationNode *conversationNode;
        for (conversationNode =
                 (struct ConversationNode *)conversation->messages->mlh_Head;
             conversationNode->node.mln_Succ != NULL;
             conversationNode =
                 (struct ConversationNode *)conversationNode->node.mln_Succ) {
            if (!strcmp(conversationNode->role, "system"))
                continue;
            struct json_object *messageJsonObject = json_object_new_object();
            json_object_object_add(
                messageJsonObject, "role",
                json_object_new_string(conversationNode->role));
            json_object_object_add(
                messageJsonObject, "content",
                json_object_new_string(
                    (CONST_STRPTR)conversationNodeGetRaw(conversationNode)));
            struct json_object *filesJsonArray = json_object_new_array();
            struct ChatFile *chatFile;
            for (chatFile =
                     (struct ChatFile *)conversationNode->files.mlh_Head;
                 chatFile->node.mln_Succ != NULL;
                 chatFile = (struct ChatFile *)chatFile->node.mln_Succ) {
                struct json_object *fileJsonObject = json_object_new_object();
                json_object_object_add(
                    fileJsonObject, "name",
                    json_object_new_string(chatFile->name));
                if (chatFile->path != NULL)
                    json_object_object_add(
                        fileJsonObject, "path",
                        json_object_new_string(chatFile->path));
                if (chatFile->mimeType != NULL)
                    json_object_object_add(
                        fileJsonObject, "mimeType",
                        json_object_new_string(chatFile->mimeType));
                if (chatFile->fileId != NULL)
                    json_object_object_add(
                        fileJsonObject, "fileId",
                        json_object_new_string(chatFile->fileId));
                if (chatFile->containerId != NULL)
                    json_object_object_add(
                        fileJsonObject, "containerId",
                        json_object_new_string(chatFile->containerId));
                if (chatFile->downloadUrl != NULL)
                    json_object_object_add(
                        fileJsonObject, "downloadUrl",
                        json_object_new_string(chatFile->downloadUrl));
                json_object_array_add(filesJsonArray, fileJsonObject);
            }
            if (json_object_array_length(filesJsonArray) > 0) {
                json_object_object_add(messageJsonObject, "files",
                                       filesJsonArray);
            } else {
                json_object_put(filesJsonArray);
            }
            json_object_array_add(messagesJsonArray, messageJsonObject);
        }
        json_object_object_add(conversationJsonObject, "messages",
                               messagesJsonArray);
        json_object_array_add(conversationsJsonArray, conversationJsonObject);
    }

    STRPTR conversationsJsonString = (STRPTR)json_object_to_json_string_ext(
        conversationsJsonArray, JSON_C_TO_STRING_PRETTY);

    if (Write(file, conversationsJsonString, strlen(conversationsJsonString)) !=
        (LONG)strlen(conversationsJsonString)) {
        displayError(STRING_ERROR_CHAT_HISTORY_SAVE);
        Close(file);
        json_object_put(conversationsJsonArray);
        return RETURN_ERROR;
    }

    Close(file);
    json_object_put(conversationsJsonArray);
    return RETURN_OK;
}

/**
 * Saves the images to disk
 * @return RETURN_OK on success, RETURN_ERROR on failure
 **/
static LONG saveImages() {
    BPTR file = Open("AMIGAGPT:image-history.json", MODE_NEWFILE);
    if (file == 0) {
        displayError(STRING_ERROR_IMAGE_HISTORY_CREATE);
        return RETURN_ERROR;
    }

    LONG totalImageCount;
    get(imageListObject, MUIA_NList_Entries, &totalImageCount);

    struct json_object *imagesJsonArray = json_object_new_array();
    for (LONG i = 0; i < totalImageCount; i++) {
        struct json_object *imageJsonObject = json_object_new_object();
        struct GeneratedImage *generatedImage;

        DoMethod(imageListObject, MUIM_NList_GetEntry, i, &generatedImage);
        json_object_object_add(imageJsonObject, "name",
                               json_object_new_string(generatedImage->name));
        json_object_object_add(
            imageJsonObject, "filePath",
            json_object_new_string(generatedImage->filePath));
        json_object_object_add(imageJsonObject, "prompt",
                               json_object_new_string(generatedImage->prompt));
        json_object_object_add(imageJsonObject, "imageModel",
                               json_object_new_int(generatedImage->imageModel));
        json_object_object_add(imageJsonObject, "width",
                               json_object_new_int(generatedImage->width));
        json_object_object_add(imageJsonObject, "height",
                               json_object_new_int(generatedImage->height));
        json_object_array_add(imagesJsonArray, imageJsonObject);
    }

    STRPTR imagesJsonString = (STRPTR)json_object_to_json_string_ext(
        imagesJsonArray, JSON_C_TO_STRING_PRETTY);

    if (Write(file, imagesJsonString, strlen(imagesJsonString)) !=
        (LONG)strlen(imagesJsonString)) {
        displayError(STRING_ERROR_IMAGE_HISTORY_WRITE);
        Close(file);
        json_object_put(imagesJsonArray);
        return RETURN_ERROR;
    }

    Close(file);
    json_object_put(imagesJsonArray);
    return RETURN_OK;
}

/**
 * Saves the generated speech phrases to disk
 * @return RETURN_OK on success, RETURN_ERROR on failure
 **/
static LONG saveSpeechHistory() {
    BPTR file = Open("AMIGAGPT:speech-history.json", MODE_NEWFILE);
    LONG totalSpeechCount;
    struct json_object *speechJsonArray;
    LONG i;
    STRPTR speechJsonString;

    if (file == 0) {
        displayError(STRING_ERROR_SPEECH_HISTORY_CREATE);
        return RETURN_ERROR;
    }

    get(speechListObject, MUIA_NList_Entries, &totalSpeechCount);
    speechJsonArray = json_object_new_array();
    for (i = 0; i < totalSpeechCount; i++) {
        struct json_object *speechJsonObject = json_object_new_object();
        struct GeneratedSpeech *entry = NULL;

        DoMethod(speechListObject, MUIM_NList_GetEntry, i, &entry);
        if (entry == NULL)
            continue;
        json_object_object_add(
            speechJsonObject, "title",
            json_object_new_string(entry->title != NULL ? entry->title : ""));
        json_object_object_add(speechJsonObject, "filePath",
                               json_object_new_string(entry->filePath != NULL
                                                          ? entry->filePath
                                                          : ""));
        json_object_object_add(
            speechJsonObject, "text",
            json_object_new_string(entry->text != NULL ? entry->text : ""));
        json_object_object_add(speechJsonObject, "profileInfo",
                               json_object_new_string(
                                   entry->profileInfo != NULL
                                       ? entry->profileInfo
                                       : ""));
        json_object_array_add(speechJsonArray, speechJsonObject);
    }

    speechJsonString = (STRPTR)json_object_to_json_string_ext(
        speechJsonArray, JSON_C_TO_STRING_PRETTY);

    if (Write(file, speechJsonString, strlen(speechJsonString)) !=
        (LONG)strlen(speechJsonString)) {
        displayError(STRING_ERROR_SPEECH_HISTORY_WRITE);
        Close(file);
        json_object_put(speechJsonArray);
        return RETURN_ERROR;
    }

    Close(file);
    json_object_put(speechJsonArray);
    return RETURN_OK;
}

/**
 * Opens and displays the image with scaling
 * @param image the image to open
 * @param width the width of the image
 * @param height the height of the image
 **/
static void openImage(struct GeneratedImage *image, WORD scaledWidth,
                      WORD scaledHeight) {
    if (image == NULL)
        return;

    // Get screen dimensions from main window
    struct Screen *currentScreen;
    get(mainWindowObject, MUIA_Window_Screen, &currentScreen);
    WORD screenWidth = currentScreen ? currentScreen->Width : 640;
    WORD screenHeight = currentScreen ? currentScreen->Height : 480;

    WORD lowestWidth =
        (screenWidth - 16) < scaledWidth ? (screenWidth - 16) : scaledWidth;
    WORD lowestHeight =
        screenHeight < scaledHeight ? screenHeight : scaledHeight;

    updateStatusBar(STRING_LOADING_IMAGE, yellowPen);

    // clang-format off
    Object *imageWindowLoadingTextObject = VGroup,
        Child, VSpace(0),
        Child, TextObject,
            MUIA_Text_Contents, STRING_LOADING_IMAGE,
            MUIA_Text_PreParse, "\033c",
        End,
        Child, VSpace(0),
    End;
    // clang-format on
    DoMethod(imageWindowImageViewGroup, OM_ADDMEMBER,
             imageWindowLoadingTextObject);
    DoMethod(imageWindowImageViewGroup, OM_REMMEMBER, imageWindowImageView);
    MUI_DisposeObject(imageWindowImageView);

    set(imageWindowObject, MUIA_Window_Title, image->name);
    set(imageWindowObject, MUIA_Window_Width, lowestWidth);
    set(imageWindowObject, MUIA_Window_Height, lowestHeight);
    set(imageWindowObject, MUIA_Window_Activate, TRUE);
    set(imageWindowObject, MUIA_Window_Open, TRUE);

    DoMethod(imageWindowImageViewGroup, MUIM_Group_InitChange);
    // clang-format off
    imageWindowImageView = GuigfxObject,
        MUIA_Guigfx_FileName, image->filePath,
        MUIA_Guigfx_Quality, MUIV_Guigfx_Quality_Best,
        MUIA_Guigfx_ScaleMode, NISMF_SCALEFREE | NISMF_KEEPASPECT_PICTURE | NISMF_KEEPASPECT_SCREEN,
        MUIA_Guigfx_Transparency, NITRF_MASK,
    End;
    // clang-format on
    DoMethod(imageWindowImageViewGroup, OM_ADDMEMBER, imageWindowImageView);
    DoMethod(imageWindowImageViewGroup, OM_REMMEMBER,
             imageWindowLoadingTextObject);
    DoMethod(imageWindowImageViewGroup, MUIM_Group_ExitChange);
    MUI_DisposeObject(imageWindowLoadingTextObject);

    updateStatusBar(STRING_READY, greenPen);
}

/**
 * Load the conversations from disk
 * @return RETURN_OK on success, RETURN_ERROR on failure
 **/
static LONG loadConversations() {
    BPTR file = Open("AMIGAGPT:chat-history.json", MODE_OLDFILE);
    if (file == 0) {
        return RETURN_OK;
    }

#ifdef __AMIGAOS3__
    Seek(file, 0, OFFSET_END);
    LONG fileSize = Seek(file, 0, OFFSET_BEGINNING);
#else
#ifdef __AMIGAOS4__
    int64_t fileSize = GetFileSize(file);
#else
    struct FileInfoBlock fib;
    ExamineFH64(file, &fib, NULL);
    int64_t fileSize = fib.fib_Size;
#endif
#endif
    STRPTR conversationsJsonString = AllocVec(fileSize + 1, MEMF_CLEAR);
    if (Read(file, conversationsJsonString, fileSize) != fileSize) {
        displayError(STRING_ERROR_CHAT_HISTORY_READ);
        Close(file);
        FreeVec(conversationsJsonString);
        return RETURN_ERROR;
    }

    Close(file);

    struct json_object *conversationsJsonArray =
        json_tokener_parse(conversationsJsonString);
    if (conversationsJsonArray == NULL ||
        !json_object_is_type(conversationsJsonArray, json_type_array)) {
        if (conversationsJsonArray != NULL) {
            json_object_put(conversationsJsonArray);
            conversationsJsonArray = NULL;
        }
        if (Rename("AMIGAGPT:chat-history.json",
                   "AMIGAGPT:chat-history.json.bak")) {
            displayError(STRING_ERROR_CHAT_HISTORY_PARSE_BACKUP);
        } else if (copyFile("AMIGAGPT:chat-history.json",
                            "RAM:chat-history.json")) {
            displayError(STRING_ERROR_CHAT_HISTORY_PARSE_BACKUP_RAM);
#if defined(__AMIGAOS3__) || defined(__MORPHOS__)
            if (!DeleteFile("AMIGAGPT:chat-history.json")) {
#else
            if (!Delete("AMIGAGPT:chat-history.json")) {
#endif
                displayError(STRING_ERROR_CHAT_HISTORY_DELETE);
            }
        }

        FreeVec(conversationsJsonString);
        return RETURN_ERROR;
    }

    set(conversationListObject, MUIA_NList_Quiet, TRUE);
    for (UWORD i = 0; i < json_object_array_length(conversationsJsonArray);
         i++) {
        struct json_object *conversationJsonObject =
            json_object_array_get_idx(conversationsJsonArray, i);
        struct json_object *conversationNameJsonObject;

        if (conversationJsonObject == NULL ||
            !json_object_is_type(conversationJsonObject, json_type_object)) {
            continue;
        }
        if (!json_object_object_get_ex(conversationJsonObject, "name",
                                       &conversationNameJsonObject)) {
            displayError(STRING_ERROR_CHAT_HISTORY_PARSE_NO_BACKUP);
            FreeVec(conversationsJsonString);
            json_object_put(conversationsJsonArray);
            set(conversationListObject, MUIA_NList_Quiet, FALSE);
            return RETURN_ERROR;
        }

        UTF8 *conversationName =
            json_object_get_string(conversationNameJsonObject);

        struct json_object *messagesJsonArray;
        if (!json_object_object_get_ex(conversationJsonObject, "messages",
                                       &messagesJsonArray) ||
            !json_object_is_type(messagesJsonArray, json_type_array)) {
            displayError(STRING_ERROR_CHAT_HISTORY_PARSE_NO_BACKUP);
            FreeVec(conversationsJsonString);
            json_object_put(conversationsJsonArray);
            set(conversationListObject, MUIA_NList_Quiet, FALSE);
            return RETURN_ERROR;
        }

        struct Conversation *conversation = newConversation();
        conversation->name = dupStringAlloc(conversationName);
        if (conversation->name == NULL) {
            freeConversation(conversation);
            displayError(STRING_ERROR_CHAT_HISTORY_PARSE_NO_BACKUP);
            FreeVec(conversationsJsonString);
            json_object_put(conversationsJsonArray);
            return RETURN_ERROR;
        }
        conversationRefreshNameListDisplay(conversation);

        struct json_object *lastResponseIdJsonObject;
        if (json_object_object_get_ex(conversationJsonObject, "lastResponseId",
                                      &lastResponseIdJsonObject)) {
            STRPTR lastResponseId =
                (STRPTR)json_object_get_string(lastResponseIdJsonObject);
            if (lastResponseId != NULL && strlen(lastResponseId) > 0) {
                conversation->lastResponseId =
                    AllocVec(strlen(lastResponseId) + 1, MEMF_ANY | MEMF_CLEAR);
                if (conversation->lastResponseId != NULL) {
                    strncpy(conversation->lastResponseId, lastResponseId,
                            strlen(lastResponseId));
                }
            }
        }

        for (UWORD j = 0; j < json_object_array_length(messagesJsonArray);
             j++) {
            struct json_object *messageJsonObject =
                json_object_array_get_idx(messagesJsonArray, j);
            struct json_object *roleJsonObject;

            if (messageJsonObject == NULL ||
                !json_object_is_type(messageJsonObject, json_type_object)) {
                continue;
            }
            if (!json_object_object_get_ex(messageJsonObject, "role",
                                           &roleJsonObject)) {
                displayError(STRING_ERROR_CHAT_HISTORY_PARSE_NO_BACKUP);
                FreeVec(conversationsJsonString);
                json_object_put(conversationsJsonArray);
                set(conversationListObject, MUIA_NList_Quiet, FALSE);
                return RETURN_ERROR;
            }
            STRPTR role = json_object_get_string(roleJsonObject);
            struct json_object *contentJsonObject;

            if (!json_object_object_get_ex(messageJsonObject, "content",
                                           &contentJsonObject)) {
                displayError(STRING_ERROR_CHAT_HISTORY_PARSE_NO_BACKUP);
                FreeVec(conversationsJsonString);
                json_object_put(conversationsJsonArray);
                set(conversationListObject, MUIA_NList_Quiet, FALSE);
                return RETURN_ERROR;
            }
            UTF8 *content = json_object_get_string(contentJsonObject);
            struct ConversationNode *message =
                addTextToConversation(conversation, content, role);
            struct json_object *filesJsonArray = NULL;
            if (message != NULL &&
                json_object_object_get_ex(messageJsonObject, "files",
                                          &filesJsonArray) &&
                json_object_is_type(filesJsonArray, json_type_array)) {
                for (ULONG k = 0;
                     k < json_object_array_length(filesJsonArray); k++) {
                    struct json_object *fileJsonObject =
                        json_object_array_get_idx(filesJsonArray, k);
                    CONST_STRPTR name =
                        jsonStringValue(fileJsonObject, "name");
                    if (name == NULL)
                        continue;
                    addChatFile(
                        &message->files,
                        jsonStringValue(fileJsonObject, "path"), name,
                        jsonStringValue(fileJsonObject, "mimeType"),
                        jsonStringValue(fileJsonObject, "fileId"),
                        jsonStringValue(fileJsonObject, "containerId"),
                        jsonStringValue(fileJsonObject, "downloadUrl"));
                }
            }
        }
        DoMethod(conversationListObject, MUIM_NList_InsertSingle, conversation,
                 MUIV_NList_Insert_Top);
    }
    set(conversationListObject, MUIA_NList_Quiet, FALSE);
    json_object_put(conversationsJsonArray);
    return RETURN_OK;
}

/**
 * Load the images from disk
 * @return RETURN_OK on success, RETURN_ERROR on failure
 **/
static LONG loadImages() {
    BPTR file = Open("AMIGAGPT:image-history.json", MODE_OLDFILE);
    if (file == 0) {
        return RETURN_OK;
    }

#ifdef __AMIGAOS3__
    Seek(file, 0, OFFSET_END);
    LONG fileSize = Seek(file, 0, OFFSET_BEGINNING);
#else
#ifdef __AMIGAOS4__
    int64_t fileSize = GetFileSize(file);
#else
    struct FileInfoBlock fib;
    ExamineFH64(file, &fib, NULL);
    int64_t fileSize = fib.fib_Size;
#endif
#endif
    STRPTR imagesJsonString = AllocVec(fileSize + 1, MEMF_CLEAR);
    if (Read(file, imagesJsonString, fileSize) != fileSize) {
        displayError(STRING_ERROR_IMAGE_HISTORY_READ);
        Close(file);
        FreeVec(imagesJsonString);
        return RETURN_ERROR;
    }

    Close(file);

    struct json_object *imagesJsonArray = json_tokener_parse(imagesJsonString);
    if (imagesJsonArray == NULL) {
        if (Rename("AMIGAGPT:image-history.json",
                   "AMIGAGPT:image-history.json.bak")) {
            displayError(STRING_ERROR_IMAGE_HISTORY_PARSE_BACKUP);
        } else if (copyFile("AMIGAGPT:image-history.json",
                            "RAM:image-history.json")) {
            displayError(STRING_ERROR_IMAGE_HISTORY_PARSE_BACKUP_RAM);
#if defined(__AMIGAOS3__) || defined(__MORPHOS__)
            if (!DeleteFile("AMIGAGPT:image-history.json")) {
#else
            if (!Delete("AMIGAGPT:image-history.json")) {
#endif
                displayError(STRING_ERROR_IMAGE_HISTORY_PARSE_NO_BACKUP);
            }
        }

        FreeVec(imagesJsonString);
        return RETURN_ERROR;
    }

    for (UWORD i = 0; i < json_object_array_length(imagesJsonArray); i++) {
        struct json_object *imageJsonObject =
            json_object_array_get_idx(imagesJsonArray, i);
        struct json_object *imageNameJsonObject;
        if (!json_object_object_get_ex(imageJsonObject, "name",
                                       &imageNameJsonObject)) {
            displayError(STRING_ERROR_IMAGE_HISTORY_PARSE_NO_BACKUP);
            FreeVec(imagesJsonString);
            json_object_put(imagesJsonArray);
            return RETURN_ERROR;
        }

        STRPTR imageName = json_object_get_string(imageNameJsonObject);

        struct json_object *imageFilePathJsonObject;
        if (!json_object_object_get_ex(imageJsonObject, "filePath",
                                       &imageFilePathJsonObject)) {
            displayError(STRING_ERROR_IMAGE_HISTORY_PARSE_NO_BACKUP);
            FreeVec(imagesJsonString);
            json_object_put(imagesJsonArray);
            return RETURN_ERROR;
        }

        STRPTR imageFilePath = json_object_get_string(imageFilePathJsonObject);

        struct json_object *imagePromptJsonObject;
        if (!json_object_object_get_ex(imageJsonObject, "prompt",
                                       &imagePromptJsonObject)) {
            displayError(STRING_ERROR_IMAGE_HISTORY_PARSE_NO_BACKUP);
            FreeVec(imagesJsonString);
            json_object_put(imagesJsonArray);
            return RETURN_ERROR;
        }

        STRPTR imagePrompt = json_object_get_string(imagePromptJsonObject);

        struct json_object *imageModelJsonObject;
        if (!json_object_object_get_ex(imageJsonObject, "imageModel",
                                       &imageModelJsonObject)) {
            displayError(STRING_ERROR_IMAGE_HISTORY_PARSE_NO_BACKUP);
            FreeVec(imagesJsonString);
            json_object_put(imagesJsonArray);
            return RETURN_ERROR;
        }

        ImageModel imageModel = json_object_get_int(imageModelJsonObject);

        struct json_object *imageWidthJsonObject;
        if (!json_object_object_get_ex(imageJsonObject, "width",
                                       &imageWidthJsonObject)) {
            displayError(STRING_ERROR_IMAGE_HISTORY_PARSE_NO_BACKUP);
            FreeVec(imagesJsonString);
            json_object_put(imagesJsonArray);
            return RETURN_ERROR;
        }

        UWORD imageWidth = (WORD)json_object_get_int(imageWidthJsonObject);

        struct json_object *imageHeightJsonObject;
        if (!json_object_object_get_ex(imageJsonObject, "height",
                                       &imageHeightJsonObject)) {
            displayError(STRING_ERROR_IMAGE_HISTORY_PARSE_NO_BACKUP);
            FreeVec(imagesJsonString);
            json_object_put(imagesJsonArray);
            return RETURN_ERROR;
        }

        UWORD imageHeight = (WORD)json_object_get_int(imageHeightJsonObject);

        struct GeneratedImage *generatedImage =
            AllocVec(sizeof(struct GeneratedImage), MEMF_ANY);
        generatedImage->name = dupStringAlloc(imageName);
        generatedImage->filePath = dupStringAlloc(imageFilePath);
        generatedImage->prompt = dupStringAlloc(imagePrompt);
        if (generatedImage->name == NULL || generatedImage->filePath == NULL ||
            generatedImage->prompt == NULL) {
            if (generatedImage->name != NULL) {
                FreeVec(generatedImage->name);
            }
            if (generatedImage->filePath != NULL) {
                FreeVec(generatedImage->filePath);
            }
            if (generatedImage->prompt != NULL) {
                FreeVec(generatedImage->prompt);
            }
            FreeVec(generatedImage);
            displayError(STRING_ERROR_IMAGE_HISTORY_PARSE_NO_BACKUP);
            FreeVec(imagesJsonString);
            json_object_put(imagesJsonArray);
            return RETURN_ERROR;
        }
        generatedImage->imageModel = imageModel;
        generatedImage->width = imageWidth;
        generatedImage->height = imageHeight;
        DoMethod(imageListObject, MUIM_NList_InsertSingle, generatedImage,
                 MUIV_NList_Insert_Top);
    }

    json_object_put(imagesJsonArray);
    return RETURN_OK;
}

/**
 * Load the generated speech phrases from disk
 * @return RETURN_OK on success, RETURN_ERROR on failure
 **/
static LONG loadSpeechHistory() {
    BPTR file = Open("AMIGAGPT:speech-history.json", MODE_OLDFILE);
    STRPTR speechJsonString;
    struct json_object *speechJsonArray;
    UWORD i;
#ifdef __AMIGAOS3__
    LONG fileSize;
#else
#ifdef __AMIGAOS4__
    int64_t fileSize;
#else
    struct FileInfoBlock fib;
    int64_t fileSize;
#endif
#endif

    if (file == 0)
        return RETURN_OK;

#ifdef __AMIGAOS3__
    Seek(file, 0, OFFSET_END);
    fileSize = Seek(file, 0, OFFSET_BEGINNING);
#else
#ifdef __AMIGAOS4__
    fileSize = GetFileSize(file);
#else
    ExamineFH64(file, &fib, NULL);
    fileSize = fib.fib_Size;
#endif
#endif
    speechJsonString = AllocVec(fileSize + 1, MEMF_CLEAR);
    if (Read(file, speechJsonString, fileSize) != fileSize) {
        displayError(STRING_ERROR_SPEECH_HISTORY_READ);
        Close(file);
        FreeVec(speechJsonString);
        return RETURN_ERROR;
    }

    Close(file);

    speechJsonArray = json_tokener_parse(speechJsonString);
    if (speechJsonArray == NULL) {
        if (Rename("AMIGAGPT:speech-history.json",
                   "AMIGAGPT:speech-history.json.bak")) {
            displayError(STRING_ERROR_SPEECH_HISTORY_PARSE_BACKUP);
        } else if (copyFile("AMIGAGPT:speech-history.json",
                            "RAM:speech-history.json")) {
            displayError(STRING_ERROR_SPEECH_HISTORY_PARSE_BACKUP_RAM);
            deleteDiskFile("AMIGAGPT:speech-history.json");
        }

        FreeVec(speechJsonString);
        return RETURN_ERROR;
    }

    for (i = 0; i < json_object_array_length(speechJsonArray); i++) {
        struct json_object *speechJsonObject =
            json_object_array_get_idx(speechJsonArray, i);
        CONST_STRPTR title = jsonStringValue(speechJsonObject, "title");
        CONST_STRPTR filePath = jsonStringValue(speechJsonObject, "filePath");
        CONST_STRPTR text = jsonStringValue(speechJsonObject, "text");
        CONST_STRPTR profileInfo =
            jsonStringValue(speechJsonObject, "profileInfo");
        struct GeneratedSpeech *entry;

        if (filePath == NULL || text == NULL) {
            displayError(STRING_ERROR_SPEECH_HISTORY_PARSE_NO_BACKUP);
            FreeVec(speechJsonString);
            json_object_put(speechJsonArray);
            return RETURN_ERROR;
        }

        entry = AllocVec(sizeof(*entry), MEMF_ANY | MEMF_CLEAR);
        if (entry == NULL)
            continue;
        entry->filePath = AllocVec(strlen(filePath) + 1, MEMF_ANY | MEMF_CLEAR);
        entry->text = AllocVec(strlen(text) + 1, MEMF_ANY | MEMF_CLEAR);
        if (title != NULL) {
            entry->title = AllocVec(strlen(title) + 1, MEMF_ANY | MEMF_CLEAR);
            if (entry->title != NULL)
                strcpy(entry->title, title);
        } else {
            entry->title = speechTitle(text);
        }
        if (entry->filePath != NULL)
            strcpy(entry->filePath, filePath);
        if (entry->text != NULL)
            strcpy(entry->text, text);
        if (profileInfo != NULL && profileInfo[0] != '\0')
            entry->profileInfo = copySpeechString(profileInfo);
        DoMethod(speechListObject, MUIM_NList_InsertSingle, entry,
                 MUIV_NList_Insert_Top);
    }

    FreeVec(speechJsonString);
    json_object_put(speechJsonArray);
    return RETURN_OK;
}

/**
 * Print the conversation text to the printer
 * @return RETURN_OK on success, RETURN_ERROR on failure
 **/
LONG printConversation() {
    BPTR printerFile;
    LONG result = RETURN_ERROR;

    printerFile = Open("PRT:", MODE_NEWFILE);
    if (printerFile) {
        STRPTR printerText =
            AllocVec(strlen(READ_BUFFER_LENGTH) + 1, MEMF_ANY | MEMF_CLEAR);
        struct ConversationNode *conversationNode;

        for (conversationNode = (struct ConversationNode *)
                                    currentConversation->messages->mlh_Head;
             conversationNode->node.mln_Succ != NULL;
             conversationNode =
                 (struct ConversationNode *)conversationNode->node.mln_Succ) {
            UTF8 *content = conversationNodeGetDisplay(conversationNode);
            if (strcmp(conversationNode->role, "user") == 0) {
                Write(printerFile, "*******************\n", -1);
                Write(printerFile, "User:\n\n", -1);

                STRPTR convertedConversationString = CodesetsUTF8ToStr(
                    CSA_DestCodeset, (Tag)systemCodeset, CSA_Source,
                    (Tag)content, CSA_MapForeignChars, TRUE, TAG_DONE);
                if (convertedConversationString != NULL) {
                    Write(printerFile, convertedConversationString, -1);
                    CodesetsFreeA(convertedConversationString, NULL);
                } else {
                    Write(printerFile, content, -1);
                }
            } else if (strcmp(conversationNode->role, "assistant") == 0) {
                Write(printerFile, "\n\n", -1);
                Write(printerFile, "*******************\n", -1);
                Write(printerFile, "AmigaGPT:\n\n", -1);

                STRPTR convertedConversationString = CodesetsUTF8ToStr(
                    CSA_DestCodeset, (Tag)systemCodeset, CSA_Source,
                    (Tag)content, CSA_MapForeignChars, TRUE, TAG_DONE);
                if (convertedConversationString != NULL) {
                    Write(printerFile, convertedConversationString, -1);
                    CodesetsFreeA(convertedConversationString, NULL);
                } else {
                    Write(printerFile, content, -1);
                }

                Write(printerFile, "\n\n", -1);
            }
        }

        FreeVec(printerText);
        Close(printerFile);
        result = RETURN_OK;
    }

    return RETURN_OK;
}

struct Conversation *getCurrentConversation(void) { return currentConversation; }
