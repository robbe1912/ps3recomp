/*
 * ps3recomp - sceNp HLE implementation
 *
 * Provides fake PSN identity so games can proceed through NP checks.
 * The username defaults to "PS3Player" but is configurable.
 */

#include "sceNp.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../runtime/ppu/ppu_memory.h"   /* vm_write*: guest EA -> host, byte-swapped */
#include "../filesystem/edat.h"             /* klicensee note-through for EDAT resolution */


/* ---------------------------------------------------------------------------
 * Internal state
 * -----------------------------------------------------------------------*/

static int  s_np_initialized = 0;
static int  s_score_initialized = 0;
static char s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH + 1] = "PS3Player";

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/

void sceNpSetFakeUsername(const char* username)
{
    if (username) {
        strncpy(s_fake_username, GUEST_PTR(username, const char*),
            SCE_NP_ONLINEID_MAX_LENGTH);
        s_fake_username[SCE_NP_ONLINEID_MAX_LENGTH] = '\0';
    }
}

/* Build a fake NP ID from the current username */
static void np_build_fake_id(SceNpId* npId)
{
    memset(npId, 0, sizeof(SceNpId));
    strncpy(npId->handle.data, s_fake_username, SCE_NP_ONLINEID_MAX_LENGTH);
    npId->handle.term = '\0';
}

/* ---------------------------------------------------------------------------
 * API implementations
 * -----------------------------------------------------------------------*/

s32 sceNpInit(u32 poolSize, void* poolPtr)
{
    (void)poolSize;
    (void)poolPtr;

    printf("[sceNp] Init(poolSize=%u, username=\"%s\")\n",
           poolSize, s_fake_username);

    if (s_np_initialized)
        return SCE_NP_ERROR_ALREADY_INITIALIZED;

    s_np_initialized = 1;
    return CELL_OK;
}

s32 sceNpTerm(void)
{
    printf("[sceNp] Term()\n");

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    s_np_initialized = 0;
    return CELL_OK;
}

s32 sceNpGetNpId(s32 userId, SceNpId* npId)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!npId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    np_build_fake_id(npId);
    printf("[sceNp] GetNpId(user=%d) -> \"%s\"\n", userId, s_fake_username);
    return CELL_OK;
}

s32 sceNpGetOnlineId(s32 userId, SceNpOnlineId* onlineId)
{
    onlineId = GUEST_PTR(onlineId, SceNpOnlineId*);
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineId)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineId, 0, sizeof(SceNpOnlineId));
    strncpy(onlineId->data, s_fake_username, SCE_NP_ONLINEID_MAX_LENGTH);
    onlineId->term = '\0';

    printf("[sceNp] GetOnlineId(user=%d) -> \"%s\"\n", userId, s_fake_username);
    return CELL_OK;
}

s32 sceNpGetOnlineName(s32 userId, SceNpOnlineName* onlineName)
{
    onlineName = GUEST_PTR(onlineName, SceNpOnlineName*);
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!onlineName)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(onlineName, 0, sizeof(SceNpOnlineName));
    strncpy(onlineName->data, s_fake_username,
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);

    printf("[sceNp] GetOnlineName(user=%d) -> \"%s\"\n",
           userId, s_fake_username);
    return CELL_OK;
}

s32 sceNpGetUserProfile(s32 userId, SceNpUserInfo* userInfo)
{
    userInfo = GUEST_PTR(userInfo, SceNpUserInfo*);
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!userInfo)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    memset(userInfo, 0, sizeof(SceNpUserInfo));
    np_build_fake_id(&userInfo->npId);
    strncpy(userInfo->onlineName.data, s_fake_username,
            SCE_NP_ONLINENAME_MAX_LENGTH - 1);
    /* Leave avatar URL empty */

    printf("[sceNp] GetUserProfile(user=%d) -> \"%s\"\n",
           userId, s_fake_username);
    return CELL_OK;
}

s32 sceNpGetAccountRegion(s32 userId, u32* region)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!region)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    /* Region: US (SCEA) = 0x5553 ('US') */
    vm_write32((u32)(uintptr_t)region, (u32)0x5553);

    printf("[sceNp] GetAccountRegion(user=%d) -> US\n", userId);
    return CELL_OK;
}

s32 sceNpGetAccountAge(s32 userId, s32* age)
{
    (void)userId;

    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!age)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    vm_write32((u32)(uintptr_t)age, 25); /* default adult age */
    printf("[sceNp] GetAccountAge(user=%d) -> 25\n", userId);
    return CELL_OK;
}

s32 sceNpGetMyLanguages(SceNpMyLanguages* langs)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;

    if (!langs)
        return SCE_NP_ERROR_INVALID_ARGUMENT;

    u32 langs_ea = (u32)(uintptr_t)langs;
    vm_write32(langs_ea + 0, SCE_NP_LANG_ENGLISH);
    vm_write32(langs_ea + 4, 0);
    vm_write32(langs_ea + 8, 0);

    printf("[sceNp] GetMyLanguages() -> English\n");
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP Manager (sign-in state)
 *
 * The toolkit runs with a fake local profile but no live PSN connection. So the
 * manager reports OFFLINE — games gate their online flows on GetStatus and skip
 * them cleanly — while the identity getters still hand back the fake account,
 * matching how NP behaves on a real signed-in-but-disconnected console.
 * Imported by most online-capable titles.
 * -----------------------------------------------------------------------*/

static SceNpManagerCallback s_npmgr_cb = NULL;
static void*                s_npmgr_cb_arg = NULL;

s32 sceNpManagerGetStatus(s32* status)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    if (!status)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    /* `status` is a GUEST address -- the HLE ABI adapter passes pointer
     * parameters straight through as guest values, so dereferencing one
     * writes to whatever host address shares that number. Same trap as
     * cellGcmSys had. Tokyo Jungle calls this during its online init. */
    vm_write32((uint32_t)(uintptr_t)status, (uint32_t)SCE_NP_MANAGER_STATUS_OFFLINE);
    printf("[sceNp] ManagerGetStatus() -> OFFLINE\n");
    return CELL_OK;
}

s32 sceNpManagerRegisterCallback(SceNpManagerCallback callback, void* arg)
{
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    /* Stored for bookkeeping; we never transition online so never fire it. */
    s_npmgr_cb     = callback;
    s_npmgr_cb_arg = arg;
    printf("[sceNp] ManagerRegisterCallback()\n");
    return CELL_OK;
}

s32 sceNpManagerUnregisterCallback(void)
{
    s_npmgr_cb     = NULL;
    s_npmgr_cb_arg = NULL;
    return CELL_OK;
}

/* Identity getters: reuse the fake-profile implementations (offline-with-account). */
s32 sceNpManagerGetNpId(SceNpId* npId)               { return sceNpGetNpId(0, npId); }
s32 sceNpManagerGetOnlineId(SceNpOnlineId* onlineId) { return sceNpGetOnlineId(0, onlineId); }
s32 sceNpManagerGetOnlineName(SceNpOnlineName* name) { return sceNpGetOnlineName(0, name); }
s32 sceNpManagerGetAccountAge(s32* age)              { return sceNpGetAccountAge(0, age); }

/* Score setup is local even while the NP manager is offline. Games may
 * initialize it before starting the worker that decides whether to use PSN. */
s32 sceNpScoreInit(void)
{
    if (s_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_ALREADY_INITIALIZED;
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_score_initialized = 1;
    return CELL_OK;
}

s32 sceNpScoreTerm(void)
{
    if (!s_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    if (!s_np_initialized)
        return SCE_NP_ERROR_NOT_INITIALIZED;
    s_score_initialized = 0;
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * NP Manager: tickets & parental controls
 *
 * The manager never leaves SCE_NP_MANAGER_STATUS_OFFLINE here, so ticket
 * requests fail with the same error a signed-out console returns and the
 * game takes its offline path. Content-rating / chat-restriction flags are
 * local parental data and read back as "unrestricted" like a retail box
 * with no parental controls set.
 * -----------------------------------------------------------------------*/

s32 sceNpManagerGetTicket(void)
{
    printf("[sceNp] ManagerGetTicket() -> OFFLINE\n");
    return SCE_NP_ERROR_OFFLINE;
}

s32 sceNpManagerRequestTicket(void)
{
    printf("[sceNp] ManagerRequestTicket() -> OFFLINE\n");
    return SCE_NP_ERROR_OFFLINE;
}

s32 sceNpManagerGetContentRatingFlag(s32 userId, u8* restriction, s32* age)
{
    (void)userId;
    printf("[sceNp] ManagerGetContentRatingFlag(userId=%d)\n", userId);
    if (!restriction || !age)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    vm_write8((u32)(uintptr_t)restriction, 0);       /* not restricted */
    vm_write32((u32)(uintptr_t)age, 25);             /* adult account  */
    return CELL_OK;
}

s32 sceNpManagerGetChatRestrictionFlag(s32 userId, u8* restriction)
{
    (void)userId;
    printf("[sceNp] ManagerGetChatRestrictionFlag(userId=%d)\n", userId);
    if (!restriction)
        return SCE_NP_ERROR_INVALID_ARGUMENT;
    vm_write8((u32)(uintptr_t)restriction, 0);
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * sceNpBasic: friend-list style entries. Without PSN there is no roster to
 * mutate or poll, so everything answers with the signed-out error.
 * -----------------------------------------------------------------------*/

s32 sceNpBasicAddPlayersHistory(void)
{
    printf("[sceNp] BasicAddPlayersHistory() -> OFFLINE\n");
    return SCE_NP_ERROR_OFFLINE;
}

s32 sceNpBasicAddFriend(void)
{
    printf("[sceNp] BasicAddFriend() -> OFFLINE\n");
    return SCE_NP_ERROR_OFFLINE;
}

s32 sceNpBasicGetBlockListEntry(void)
{
    printf("[sceNp] BasicGetBlockListEntry() -> OFFLINE\n");
    return SCE_NP_ERROR_OFFLINE;
}

s32 sceNpBasicRecvMessageAttachment(void)
{
    printf("[sceNp] BasicRecvMessageAttachment() -> OFFLINE\n");
    return SCE_NP_ERROR_OFFLINE;
}

/* NPDRM license check. Firmware ABI: r3 is the npDrm context the game built
 * earlier, r4 points at the 16-byte klicensee. For a retail disc image the
 * license is available by construction -- but the klicensee is worth keeping:
 * the same title's HDD-installed EDAT content (MotorStorm's pack_toc.edat
 * etc.) is license-type-3 and keyed by exactly these 16 bytes, and every
 * cellFsOpen of it goes through edat_resolve. Note it there; it is validated
 * per file against the NPD dev_hash before any use. */
extern uint32_t ppu_vm_size;   /* ppu_loader.cpp, extern "C"; 0 = full space backed */

s32 sceNpDrmIsAvailable(u32 npdrm_ea, u32 klic_ea)
{
    if (npdrm_ea && (!ppu_vm_size || (uint64_t)npdrm_ea + 16 <= ppu_vm_size)) {
        uint8_t klic[16], path[16];
        /* Ground truth from this title's calls: r3 carries the 16 raw
         * klicensee bytes (high entropy), r4 an ASCII content path
         * ("/dev_hdd0/game/B.."). Note the key from r3. */
        memcpy(klic, GUEST_PTR(npdrm_ea, const uint8_t*), 16);   /* raw, no swap */
        memcpy(path, GUEST_PTR(klic_ea, const uint8_t*), 16);    /* r4, diagnostic only */
        edat_note_guest_klic(klic);
        printf("[sceNp] DrmIsAvailable: noted klic ea=0x%08X "
               "r3=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X "
               "r4=%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X%02X\n",
               npdrm_ea, klic[0],klic[1],klic[2],klic[3],klic[4],klic[5],klic[6],klic[7],
               klic[8],klic[9],klic[10],klic[11],klic[12],klic[13],klic[14],klic[15],
               path[0],path[1],path[2],path[3],path[4],path[5],path[6],path[7],
               path[8],path[9],path[10],path[11],path[12],path[13],path[14],path[15]);
    } else {
        printf("[sceNp] DrmIsAvailable() -> CELL_OK (retail disc image)\n");
    }
    return CELL_OK;
}

/* ---------------------------------------------------------------------------
 * Score contexts. Creation succeeds while score is initialized (the ladder
 * worker may build contexts before deciding to go online) and every network
 * operation fails immediately with OFFLINE, surfaced through Poll/WaitAsync
 * exactly like a request the server refused.
 * -----------------------------------------------------------------------*/

static u32 s_score_title_ctx = 0x11110001u;
static u32 s_score_trans_ctx = 0x22220001u;

s32 sceNpScoreCreateTitleCtx(void)
{
    if (!s_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    printf("[sceNp] ScoreCreateTitleCtx() -> 0x%08X\n", s_score_title_ctx);
    return (s32)s_score_title_ctx;
}

s32 sceNpScoreDestroyTitleCtx(u32 ctx)
{
    (void)ctx;
    return CELL_OK;
}

s32 sceNpScoreCreateTransactionCtx(u32 titleCtx)
{
    if (!s_score_initialized)
        return SCE_NP_COMMUNITY_ERROR_NOT_INITIALIZED;
    (void)titleCtx;
    printf("[sceNp] ScoreCreateTransactionCtx() -> 0x%08X\n", s_score_trans_ctx);
    return (s32)s_score_trans_ctx;
}

s32 sceNpScoreDestroyTransactionCtx(u32 ctx, u32 transCtx)
{
    (void)ctx;
    (void)transCtx;
    return CELL_OK;
}

s32 sceNpScoreRecordScoreAsync(void)      { return SCE_NP_ERROR_OFFLINE; }
s32 sceNpScoreRecordGameDataAsync(void)   { return SCE_NP_ERROR_OFFLINE; }
s32 sceNpScoreGetRankingByRangeAsync(void){ return SCE_NP_ERROR_OFFLINE; }
s32 sceNpScoreGetRankingByNpIdAsync(void) { return SCE_NP_ERROR_OFFLINE; }
s32 sceNpScoreGetGameDataAsync(void)      { return SCE_NP_ERROR_OFFLINE; }

s32 sceNpScorePollAsync(u32 ctx, u32 reqId, s32* result)
{
    (void)ctx;
    (void)reqId;
    printf("[sceNp] ScorePollAsync(reqId=%u) -> OFFLINE\n", reqId);
    if (result)
        vm_write32((u32)(uintptr_t)result, (u32)SCE_NP_ERROR_OFFLINE);
    return CELL_OK;
}

s32 sceNpScoreWaitAsync(u32 ctx, u32 reqId, s32* result)
{
    return sceNpScorePollAsync(ctx, reqId, result);
}

s32 sceNpScoreAbortTransaction(u32 transCtx)
{
    (void)transCtx;
    return CELL_OK;
}
