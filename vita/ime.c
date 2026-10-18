#include "ime.h"

#include <string.h>

#include <psp2/apputil.h>
#include <psp2/common_dialog.h>
#include <psp2/ime_dialog.h>
#include <psp2/system_param.h>

#include "log.h"
#include "text.h"

static uint16_t title16[SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1];
static uint16_t initial16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static uint16_t input16[SCE_IME_DIALOG_MAX_TEXT_LENGTH + 1];
static int active;

void ime_setup(void)
{
    SceAppUtilInitParam init;
    SceAppUtilBootParam boot;
    SceCommonDialogConfigParam cfg;
    int lang = SCE_SYSTEM_PARAM_LANG_ENGLISH_GB, enter = SCE_SYSTEM_PARAM_ENTER_BUTTON_CROSS;

    memset(&init, 0, sizeof(init));
    memset(&boot, 0, sizeof(boot));
    sceAppUtilInit(&init, &boot);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_LANG, &lang);
    sceAppUtilSystemParamGetInt(SCE_SYSTEM_PARAM_ID_ENTER_BUTTON, &enter);

    sceCommonDialogConfigParamInit(&cfg);
    cfg.language = (SceSystemParamLang)lang;
    cfg.enterButtonAssign = (SceSystemParamEnterButtonAssign)enter;
    sceCommonDialogSetConfigParam(&cfg);
}

int ime_open(const char *title, const char *initial, ime_kind kind, int max_len)
{
    SceImeDialogParam p;
    int rc;

    if (active)
        return -1;
    if (max_len > SCE_IME_DIALOG_MAX_TEXT_LENGTH)
        max_len = SCE_IME_DIALOG_MAX_TEXT_LENGTH;
    utf8_to_utf16(title, title16, SCE_IME_DIALOG_MAX_TITLE_LENGTH + 1);
    utf8_to_utf16(initial ? initial : "", initial16, (size_t)max_len + 1);
    memset(input16, 0, sizeof(input16));

    sceImeDialogParamInit(&p);
    p.supportedLanguages = SCE_IME_LANGUAGE_ENGLISH;
    p.languagesForced = SCE_FALSE;
    p.type = kind == IME_URL ? SCE_IME_TYPE_URL
           : kind == IME_NUMBER ? SCE_IME_TYPE_EXTENDED_NUMBER
           : SCE_IME_TYPE_DEFAULT;
    p.option = 0;
    /* Without this the dialog has no way to back out. */
    p.dialogMode = SCE_IME_DIALOG_DIALOG_MODE_WITH_CANCEL;
    p.textBoxMode = SCE_IME_DIALOG_TEXTBOX_MODE_WITH_CLEAR;
    p.title = title16;
    p.maxTextLength = (SceUInt32)max_len;
    p.initialText = initial16;
    p.inputTextBuffer = input16;

    rc = sceImeDialogInit(&p);
    if (rc < 0) {
        LOG("ime: init failed 0x%08X", rc);
        return -1;
    }
    active = 1;
    return 0;
}

int ime_active(void)
{
    return active;
}

void ime_cancel(void)
{
    if (active)
        sceImeDialogAbort();
}

int ime_poll(char *out, size_t n)
{
    SceImeDialogResult res;

    if (!active)
        return -1;
    if (sceImeDialogGetStatus() != SCE_COMMON_DIALOG_STATUS_FINISHED)
        return 0;

    memset(&res, 0, sizeof(res));
    sceImeDialogGetResult(&res);
    sceImeDialogTerm();
    active = 0;
    if (res.button != SCE_IME_DIALOG_BUTTON_ENTER) {
        LOG("ime: cancelled (button %d)", (int)res.button);
        return -1;
    }
    utf16_to_utf8(input16, out, n);
    return 1;
}
