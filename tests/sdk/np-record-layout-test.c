/* Compile-time ABI guard for <cell/np.h>: the system's network platform services
 * exchange these records with user space, so every size, alignment and offset
 * is part of the system interface.  Both data models use the system's 32-bit
 * layout; pointer members are 32-bit effective addresses (ATTRIBUTE_PRXPTR). */
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <cell/np.h>

#ifdef __cplusplus
#define CHECK(c, m) static_assert(c, m)
#else
#define CHECK(c, m) _Static_assert(c, m)
#endif
#define SIZE(t, n) CHECK(sizeof(t) == (n), #t " size")
#define ALIGN(t, n) CHECK(__alignof__(t) == (n), #t " alignment")
#define OFFSET(t, f, n) CHECK(offsetof(t, f) == (n), #t "." #f " offset")
#define WIDTH(t, f, n) CHECK(sizeof(((t *)0)->f) == (n), #t "." #f " width")

/* The runner passes the data model it asked for; prove the compiler used it. */
CHECK(sizeof(void *) == EXPECTED_ABI_BYTES, "actual pointer data model");
CHECK(sizeof(long) == EXPECTED_ABI_BYTES, "actual long data model");

/* SceNpUserInfo2 */
SIZE(SceNpUserInfo2, 44);
ALIGN(SceNpUserInfo2, 4);
OFFSET(SceNpUserInfo2, npId, 0);
WIDTH(SceNpUserInfo2, npId, 36);
OFFSET(SceNpUserInfo2, onlineName, 36);
WIDTH(SceNpUserInfo2, onlineName, 4);
OFFSET(SceNpUserInfo2, avatarUrl, 40);
WIDTH(SceNpUserInfo2, avatarUrl, 4);

/* SceNpClansClanBasicInfo */
SIZE(SceNpClansClanBasicInfo, 84);
ALIGN(SceNpClansClanBasicInfo, 4);
OFFSET(SceNpClansClanBasicInfo, clanId, 0);
WIDTH(SceNpClansClanBasicInfo, clanId, 4);
OFFSET(SceNpClansClanBasicInfo, numMembers, 4);
WIDTH(SceNpClansClanBasicInfo, numMembers, 4);
OFFSET(SceNpClansClanBasicInfo, name, 8);
WIDTH(SceNpClansClanBasicInfo, name, 65);
OFFSET(SceNpClansClanBasicInfo, tag, 73);
WIDTH(SceNpClansClanBasicInfo, tag, 9);
OFFSET(SceNpClansClanBasicInfo, reserved, 82);
WIDTH(SceNpClansClanBasicInfo, reserved, 2);

/* SceNpClansMessageData */
SIZE(SceNpClansMessageData, 8);
ALIGN(SceNpClansMessageData, 4);
OFFSET(SceNpClansMessageData, binData1, 0);
WIDTH(SceNpClansMessageData, binData1, 4);
OFFSET(SceNpClansMessageData, binData1Size, 4);
WIDTH(SceNpClansMessageData, binData1Size, 4);

/* SceNpClansUpdatableClanInfo */
SIZE(SceNpClansUpdatableClanInfo, 476);
ALIGN(SceNpClansUpdatableClanInfo, 4);
OFFSET(SceNpClansUpdatableClanInfo, fields, 0);
WIDTH(SceNpClansUpdatableClanInfo, fields, 4);
OFFSET(SceNpClansUpdatableClanInfo, description, 4);
WIDTH(SceNpClansUpdatableClanInfo, description, 256);
OFFSET(SceNpClansUpdatableClanInfo, attr, 260);
WIDTH(SceNpClansUpdatableClanInfo, attr, 208);
OFFSET(SceNpClansUpdatableClanInfo, binData1, 468);
WIDTH(SceNpClansUpdatableClanInfo, binData1, 4);
OFFSET(SceNpClansUpdatableClanInfo, binData1Size, 472);
WIDTH(SceNpClansUpdatableClanInfo, binData1Size, 4);

/* SceNpClansClanInfo */
SIZE(SceNpClansClanInfo, 568);
ALIGN(SceNpClansClanInfo, 8);
OFFSET(SceNpClansClanInfo, dateCreated, 0);
WIDTH(SceNpClansClanInfo, dateCreated, 8);
OFFSET(SceNpClansClanInfo, info, 8);
WIDTH(SceNpClansClanInfo, info, 84);
OFFSET(SceNpClansClanInfo, updatable, 92);
WIDTH(SceNpClansClanInfo, updatable, 476);

/* SceNpClansUpdatableMemberInfo */
SIZE(SceNpClansUpdatableMemberInfo, 288);
ALIGN(SceNpClansUpdatableMemberInfo, 4);
OFFSET(SceNpClansUpdatableMemberInfo, fields, 0);
WIDTH(SceNpClansUpdatableMemberInfo, fields, 4);
OFFSET(SceNpClansUpdatableMemberInfo, binData1, 4);
WIDTH(SceNpClansUpdatableMemberInfo, binData1, 4);
OFFSET(SceNpClansUpdatableMemberInfo, binData1Size, 8);
WIDTH(SceNpClansUpdatableMemberInfo, binData1Size, 4);
OFFSET(SceNpClansUpdatableMemberInfo, binAttr1, 12);
WIDTH(SceNpClansUpdatableMemberInfo, binAttr1, 16);
OFFSET(SceNpClansUpdatableMemberInfo, description, 28);
WIDTH(SceNpClansUpdatableMemberInfo, description, 256);
OFFSET(SceNpClansUpdatableMemberInfo, allowMsg, 284);
WIDTH(SceNpClansUpdatableMemberInfo, allowMsg, 1);
OFFSET(SceNpClansUpdatableMemberInfo, reserved, 285);
WIDTH(SceNpClansUpdatableMemberInfo, reserved, 3);

/* SceNpClansMemberEntry */
SIZE(SceNpClansMemberEntry, 332);
ALIGN(SceNpClansMemberEntry, 4);
OFFSET(SceNpClansMemberEntry, npid, 0);
WIDTH(SceNpClansMemberEntry, npid, 36);
OFFSET(SceNpClansMemberEntry, role, 36);
WIDTH(SceNpClansMemberEntry, role, 4);
OFFSET(SceNpClansMemberEntry, status, 40);
WIDTH(SceNpClansMemberEntry, status, 4);
OFFSET(SceNpClansMemberEntry, updatable, 44);
WIDTH(SceNpClansMemberEntry, updatable, 288);

/* SceNpClansMessageEntry */
SIZE(SceNpClansMessageEntry, 1656);
ALIGN(SceNpClansMessageEntry, 8);
OFFSET(SceNpClansMessageEntry, postDate, 0);
WIDTH(SceNpClansMessageEntry, postDate, 8);
OFFSET(SceNpClansMessageEntry, mId, 8);
WIDTH(SceNpClansMessageEntry, mId, 4);
OFFSET(SceNpClansMessageEntry, message, 12);
WIDTH(SceNpClansMessageEntry, message, 1596);
OFFSET(SceNpClansMessageEntry, data, 1608);
WIDTH(SceNpClansMessageEntry, data, 8);
OFFSET(SceNpClansMessageEntry, npid, 1616);
WIDTH(SceNpClansMessageEntry, npid, 36);
OFFSET(SceNpClansMessageEntry, postedBy, 1652);
WIDTH(SceNpClansMessageEntry, postedBy, 4);

/* SceNpCommerce2CommonData */
SIZE(SceNpCommerce2CommonData, 40);
ALIGN(SceNpCommerce2CommonData, 4);
OFFSET(SceNpCommerce2CommonData, version, 0);
WIDTH(SceNpCommerce2CommonData, version, 4);
OFFSET(SceNpCommerce2CommonData, buf_head, 4);
WIDTH(SceNpCommerce2CommonData, buf_head, 4);
OFFSET(SceNpCommerce2CommonData, bufHead, 4);
WIDTH(SceNpCommerce2CommonData, bufHead, 4);
OFFSET(SceNpCommerce2CommonData, buf_size, 8);
WIDTH(SceNpCommerce2CommonData, buf_size, 4);
OFFSET(SceNpCommerce2CommonData, bufSize, 8);
WIDTH(SceNpCommerce2CommonData, bufSize, 4);
OFFSET(SceNpCommerce2CommonData, data, 12);
WIDTH(SceNpCommerce2CommonData, data, 4);
OFFSET(SceNpCommerce2CommonData, data_size, 16);
WIDTH(SceNpCommerce2CommonData, data_size, 4);
OFFSET(SceNpCommerce2CommonData, dataSize, 16);
WIDTH(SceNpCommerce2CommonData, dataSize, 4);
OFFSET(SceNpCommerce2CommonData, data2, 20);
WIDTH(SceNpCommerce2CommonData, data2, 4);
OFFSET(SceNpCommerce2CommonData, ov, 24);
WIDTH(SceNpCommerce2CommonData, ov, 4);
OFFSET(SceNpCommerce2CommonData, reserved, 28);
WIDTH(SceNpCommerce2CommonData, reserved, 12);

/* SceNpCommerce2ContentRatingDescriptor */
SIZE(SceNpCommerce2ContentRatingDescriptor, 52);
ALIGN(SceNpCommerce2ContentRatingDescriptor, 4);
OFFSET(SceNpCommerce2ContentRatingDescriptor, commonData, 0);
WIDTH(SceNpCommerce2ContentRatingDescriptor, commonData, 40);
OFFSET(SceNpCommerce2ContentRatingDescriptor, descriptorType, 40);
WIDTH(SceNpCommerce2ContentRatingDescriptor, descriptorType, 4);
OFFSET(SceNpCommerce2ContentRatingDescriptor, imageUrl, 44);
WIDTH(SceNpCommerce2ContentRatingDescriptor, imageUrl, 4);
OFFSET(SceNpCommerce2ContentRatingDescriptor, contentRatingDescription, 48);
WIDTH(SceNpCommerce2ContentRatingDescriptor, contentRatingDescription, 4);

/* SceNpCommerce2ContentRatingInfo */
SIZE(SceNpCommerce2ContentRatingInfo, 52);
ALIGN(SceNpCommerce2ContentRatingInfo, 4);
OFFSET(SceNpCommerce2ContentRatingInfo, commonData, 0);
WIDTH(SceNpCommerce2ContentRatingInfo, commonData, 40);
OFFSET(SceNpCommerce2ContentRatingInfo, ratingSystemId, 40);
WIDTH(SceNpCommerce2ContentRatingInfo, ratingSystemId, 4);
OFFSET(SceNpCommerce2ContentRatingInfo, imageUrl, 44);
WIDTH(SceNpCommerce2ContentRatingInfo, imageUrl, 4);
OFFSET(SceNpCommerce2ContentRatingInfo, countOfContentRatingDescriptor, 48);
WIDTH(SceNpCommerce2ContentRatingInfo, countOfContentRatingDescriptor, 4);

/* SceNpCommerce2CategoryInfo */
SIZE(SceNpCommerce2CategoryInfo, 80);
ALIGN(SceNpCommerce2CategoryInfo, 8);
OFFSET(SceNpCommerce2CategoryInfo, commonData, 0);
WIDTH(SceNpCommerce2CategoryInfo, commonData, 40);
OFFSET(SceNpCommerce2CategoryInfo, dataType, 40);
WIDTH(SceNpCommerce2CategoryInfo, dataType, 4);
OFFSET(SceNpCommerce2CategoryInfo, categoryId, 44);
WIDTH(SceNpCommerce2CategoryInfo, categoryId, 4);
OFFSET(SceNpCommerce2CategoryInfo, releaseDate, 48);
WIDTH(SceNpCommerce2CategoryInfo, releaseDate, 8);
OFFSET(SceNpCommerce2CategoryInfo, categoryName, 56);
WIDTH(SceNpCommerce2CategoryInfo, categoryName, 4);
OFFSET(SceNpCommerce2CategoryInfo, categoryDescription, 60);
WIDTH(SceNpCommerce2CategoryInfo, categoryDescription, 4);
OFFSET(SceNpCommerce2CategoryInfo, imageUrl, 64);
WIDTH(SceNpCommerce2CategoryInfo, imageUrl, 4);
OFFSET(SceNpCommerce2CategoryInfo, spName, 68);
WIDTH(SceNpCommerce2CategoryInfo, spName, 4);
OFFSET(SceNpCommerce2CategoryInfo, countOfSubCategory, 72);
WIDTH(SceNpCommerce2CategoryInfo, countOfSubCategory, 4);
OFFSET(SceNpCommerce2CategoryInfo, countOfProduct, 76);
WIDTH(SceNpCommerce2CategoryInfo, countOfProduct, 4);

/* SceNpCommerce2GameProductInfo */
SIZE(SceNpCommerce2GameProductInfo, 128);
ALIGN(SceNpCommerce2GameProductInfo, 8);
OFFSET(SceNpCommerce2GameProductInfo, commonData, 0);
WIDTH(SceNpCommerce2GameProductInfo, commonData, 40);
OFFSET(SceNpCommerce2GameProductInfo, dataType, 40);
WIDTH(SceNpCommerce2GameProductInfo, dataType, 4);
OFFSET(SceNpCommerce2GameProductInfo, productId, 44);
WIDTH(SceNpCommerce2GameProductInfo, productId, 4);
OFFSET(SceNpCommerce2GameProductInfo, countOfSku, 48);
WIDTH(SceNpCommerce2GameProductInfo, countOfSku, 4);
OFFSET(SceNpCommerce2GameProductInfo, padding, 52);
WIDTH(SceNpCommerce2GameProductInfo, padding, 4);
OFFSET(SceNpCommerce2GameProductInfo, releaseDate, 56);
WIDTH(SceNpCommerce2GameProductInfo, releaseDate, 8);
OFFSET(SceNpCommerce2GameProductInfo, productName, 64);
WIDTH(SceNpCommerce2GameProductInfo, productName, 4);
OFFSET(SceNpCommerce2GameProductInfo, productShortDescription, 68);
WIDTH(SceNpCommerce2GameProductInfo, productShortDescription, 4);
OFFSET(SceNpCommerce2GameProductInfo, imageUrl, 72);
WIDTH(SceNpCommerce2GameProductInfo, imageUrl, 4);
OFFSET(SceNpCommerce2GameProductInfo, spName, 76);
WIDTH(SceNpCommerce2GameProductInfo, spName, 4);
OFFSET(SceNpCommerce2GameProductInfo, productLongDescription, 80);
WIDTH(SceNpCommerce2GameProductInfo, productLongDescription, 4);
OFFSET(SceNpCommerce2GameProductInfo, legalDescription, 84);
WIDTH(SceNpCommerce2GameProductInfo, legalDescription, 4);
OFFSET(SceNpCommerce2GameProductInfo, productAttr, 88);
WIDTH(SceNpCommerce2GameProductInfo, productAttr, 4);
OFFSET(SceNpCommerce2GameProductInfo, reserved, 92);
WIDTH(SceNpCommerce2GameProductInfo, reserved, 36);

/* SceNpCommerce2GameSkuInfo */
SIZE(SceNpCommerce2GameSkuInfo, 176);
ALIGN(SceNpCommerce2GameSkuInfo, 8);
OFFSET(SceNpCommerce2GameSkuInfo, commonData, 0);
WIDTH(SceNpCommerce2GameSkuInfo, commonData, 40);
OFFSET(SceNpCommerce2GameSkuInfo, dataType, 40);
WIDTH(SceNpCommerce2GameSkuInfo, dataType, 4);
OFFSET(SceNpCommerce2GameSkuInfo, skuId, 44);
WIDTH(SceNpCommerce2GameSkuInfo, skuId, 4);
OFFSET(SceNpCommerce2GameSkuInfo, skuType, 48);
WIDTH(SceNpCommerce2GameSkuInfo, skuType, 4);
OFFSET(SceNpCommerce2GameSkuInfo, countUntilExpiration, 52);
WIDTH(SceNpCommerce2GameSkuInfo, countUntilExpiration, 4);
OFFSET(SceNpCommerce2GameSkuInfo, timeUntilExpiration, 56);
WIDTH(SceNpCommerce2GameSkuInfo, timeUntilExpiration, 4);
OFFSET(SceNpCommerce2GameSkuInfo, purchasabilityFlag, 60);
WIDTH(SceNpCommerce2GameSkuInfo, purchasabilityFlag, 4);
OFFSET(SceNpCommerce2GameSkuInfo, annotation, 64);
WIDTH(SceNpCommerce2GameSkuInfo, annotation, 4);
OFFSET(SceNpCommerce2GameSkuInfo, downloadable, 68);
WIDTH(SceNpCommerce2GameSkuInfo, downloadable, 1);
OFFSET(SceNpCommerce2GameSkuInfo, padding, 69);
WIDTH(SceNpCommerce2GameSkuInfo, padding, 3);
OFFSET(SceNpCommerce2GameSkuInfo, price, 72);
WIDTH(SceNpCommerce2GameSkuInfo, price, 4);
OFFSET(SceNpCommerce2GameSkuInfo, skuName, 76);
WIDTH(SceNpCommerce2GameSkuInfo, skuName, 4);
OFFSET(SceNpCommerce2GameSkuInfo, productId, 80);
WIDTH(SceNpCommerce2GameSkuInfo, productId, 4);
OFFSET(SceNpCommerce2GameSkuInfo, contentLinkUrl, 84);
WIDTH(SceNpCommerce2GameSkuInfo, contentLinkUrl, 4);
OFFSET(SceNpCommerce2GameSkuInfo, countOfRewardInfo, 88);
WIDTH(SceNpCommerce2GameSkuInfo, countOfRewardInfo, 4);
OFFSET(SceNpCommerce2GameSkuInfo, skuAttr, 92);
WIDTH(SceNpCommerce2GameSkuInfo, skuAttr, 4);
OFFSET(SceNpCommerce2GameSkuInfo, salesType, 96);
WIDTH(SceNpCommerce2GameSkuInfo, salesType, 4);
OFFSET(SceNpCommerce2GameSkuInfo, firstPlayExpiration, 100);
WIDTH(SceNpCommerce2GameSkuInfo, firstPlayExpiration, 4);
OFFSET(SceNpCommerce2GameSkuInfo, countOfApplicableRewardInfo, 104);
WIDTH(SceNpCommerce2GameSkuInfo, countOfApplicableRewardInfo, 4);
OFFSET(SceNpCommerce2GameSkuInfo, countOfGameSkuEntitlementInfo, 108);
WIDTH(SceNpCommerce2GameSkuInfo, countOfGameSkuEntitlementInfo, 4);
OFFSET(SceNpCommerce2GameSkuInfo, playableDate, 112);
WIDTH(SceNpCommerce2GameSkuInfo, playableDate, 8);
OFFSET(SceNpCommerce2GameSkuInfo, chargeDate, 120);
WIDTH(SceNpCommerce2GameSkuInfo, chargeDate, 8);
OFFSET(SceNpCommerce2GameSkuInfo, chargeModel, 128);
WIDTH(SceNpCommerce2GameSkuInfo, chargeModel, 4);
OFFSET(SceNpCommerce2GameSkuInfo, augmentedDescription, 132);
WIDTH(SceNpCommerce2GameSkuInfo, augmentedDescription, 4);
OFFSET(SceNpCommerce2GameSkuInfo, reserved, 136);
WIDTH(SceNpCommerce2GameSkuInfo, reserved, 40);

/* SceNpCommerce2SessionInfo */
SIZE(SceNpCommerce2SessionInfo, 44);
ALIGN(SceNpCommerce2SessionInfo, 4);
OFFSET(SceNpCommerce2SessionInfo, currencyCode, 0);
WIDTH(SceNpCommerce2SessionInfo, currencyCode, 4);
OFFSET(SceNpCommerce2SessionInfo, decimals, 4);
WIDTH(SceNpCommerce2SessionInfo, decimals, 4);
OFFSET(SceNpCommerce2SessionInfo, currencySymbol, 8);
WIDTH(SceNpCommerce2SessionInfo, currencySymbol, 4);
OFFSET(SceNpCommerce2SessionInfo, symbolPosition, 12);
WIDTH(SceNpCommerce2SessionInfo, symbolPosition, 4);
OFFSET(SceNpCommerce2SessionInfo, symbolWithSpace, 16);
WIDTH(SceNpCommerce2SessionInfo, symbolWithSpace, 1);
OFFSET(SceNpCommerce2SessionInfo, thousandSeparator, 17);
WIDTH(SceNpCommerce2SessionInfo, thousandSeparator, 5);
OFFSET(SceNpCommerce2SessionInfo, decimalLetter, 22);
WIDTH(SceNpCommerce2SessionInfo, decimalLetter, 5);
OFFSET(SceNpCommerce2SessionInfo, padding, 27);
WIDTH(SceNpCommerce2SessionInfo, padding, 1);
OFFSET(SceNpCommerce2SessionInfo, reserved, 28);
WIDTH(SceNpCommerce2SessionInfo, reserved, 16);

/* SceNpMatching2UserInfo */
SIZE(SceNpMatching2UserInfo, 64);
ALIGN(SceNpMatching2UserInfo, 4);
OFFSET(SceNpMatching2UserInfo, next, 0);
WIDTH(SceNpMatching2UserInfo, next, 4);
OFFSET(SceNpMatching2UserInfo, userInfo, 4);
WIDTH(SceNpMatching2UserInfo, userInfo, 44);
OFFSET(SceNpMatching2UserInfo, userBinAttr, 48);
WIDTH(SceNpMatching2UserInfo, userBinAttr, 4);
OFFSET(SceNpMatching2UserInfo, userBinAttrNum, 52);
WIDTH(SceNpMatching2UserInfo, userBinAttrNum, 4);
OFFSET(SceNpMatching2UserInfo, joinedSessionInfo, 56);
WIDTH(SceNpMatching2UserInfo, joinedSessionInfo, 4);
OFFSET(SceNpMatching2UserInfo, joinedSessionInfoNum, 60);
WIDTH(SceNpMatching2UserInfo, joinedSessionInfoNum, 4);

/* Real objects, so every record is laid out by the compiler, not only measured. */
SceNpUserInfo2 np_layout_user2;
SceNpClansClanBasicInfo np_layout_clan_basic;
SceNpClansClanInfo np_layout_clan;
SceNpClansMemberEntry np_layout_member;
SceNpCommerce2GameSkuInfo np_layout_sku;
SceNpMatching2UserInfo np_layout_m2_user;

/* Pointer type semantics check: ensure pointer fields reject integer assignment
 * and have the exact expected pointer types in both ILP32 and LP64. */
void np_pointer_type_semantics_check(void);
void np_pointer_type_semantics_check(void) {
    SceNpUserInfo2 u2;
    SceNpOnlineName *p_on = (SceNpOnlineName *)0;
    SceNpAvatarUrl *p_av = (SceNpAvatarUrl *)0;
    u2.onlineName = p_on;
    u2.avatarUrl = p_av;
    p_on = u2.onlineName;
    p_av = u2.avatarUrl;
    (void)p_on; (void)p_av;

    SceNpMatching2UserInfo m2;
    struct SceNpMatching2UserInfo *p_m2 = (struct SceNpMatching2UserInfo *)0;
    SceNpMatching2BinAttr *p_ba = (SceNpMatching2BinAttr *)0;
    SceNpMatching2JoinedSessionInfo *p_js = (SceNpMatching2JoinedSessionInfo *)0;
    m2.next = p_m2;
    m2.userBinAttr = p_ba;
    m2.joinedSessionInfo = p_js;
    p_m2 = m2.next;
    p_ba = m2.userBinAttr;
    p_js = m2.joinedSessionInfo;
    (void)p_m2; (void)p_ba; (void)p_js;

    SceNpClansUpdatableClanInfo uc;
    void *p_v = (void *)0;
    uc.binData1 = p_v;
    p_v = uc.binData1;
    (void)p_v;

    SceNpCommerce2CommonData cd;
    cd.buf_head = p_v;
    cd.bufHead = p_v;
    cd.data = p_v;
    cd.data2 = p_v;
    p_v = cd.buf_head;
    p_v = cd.bufHead;
    p_v = cd.data;
    p_v = cd.data2;

    SceNpCommerce2CategoryInfo cat;
    const char *p_c = (const char *)0;
    cat.categoryId = p_c;
    cat.categoryName = p_c;
    cat.categoryDescription = p_c;
    cat.imageUrl = p_c;
    cat.spName = p_c;
    p_c = cat.categoryId;
    p_c = cat.categoryName;
    p_c = cat.categoryDescription;
    p_c = cat.imageUrl;
    p_c = cat.spName;

    SceNpCommerce2GameProductInfo prod;
    prod.productId = p_c;
    prod.productName = p_c;
    prod.productShortDescription = p_c;
    prod.imageUrl = p_c;
    prod.spName = p_c;
    prod.productLongDescription = p_c;
    prod.legalDescription = p_c;
    p_c = prod.productId;
    p_c = prod.productName;
    p_c = prod.productShortDescription;
    p_c = prod.imageUrl;
    p_c = prod.spName;
    p_c = prod.productLongDescription;
    p_c = prod.legalDescription;

    SceNpCommerce2ContentRatingInfo cr;
    cr.ratingSystemId = p_c;
    cr.imageUrl = p_c;
    p_c = cr.ratingSystemId;
    p_c = cr.imageUrl;

    SceNpCommerce2ContentRatingDescriptor crd;
    crd.imageUrl = p_c;
    crd.contentRatingDescription = p_c;
    p_c = crd.imageUrl;
    p_c = crd.contentRatingDescription;

    SceNpCommerce2GameSkuInfo sku;
    sku.skuId = p_c;
    sku.price = p_c;
    sku.skuName = p_c;
    sku.productId = p_c;
    sku.contentLinkUrl = p_c;
    sku.augmentedDescription = p_c;
    p_c = sku.skuId;
    p_c = sku.price;
    p_c = sku.skuName;
    p_c = sku.productId;
    p_c = sku.contentLinkUrl;
    p_c = sku.augmentedDescription;
    (void)p_c;
}

#ifdef __cplusplus
extern "C" {
#endif
void np_witness_decode_mismatch(void);
#ifdef __cplusplus
}
#endif

#define NP_WITNESS_CHECK(e) do { if (!(e)) np_witness_decode_mismatch(); } while (0)

/* Byte-buffer boundary witness: simulate byte buffers filled with 32-bit big-endian
 * pointers and counts as populated by the system service.  Verify that field access
 * through the record pointers reads the exact expected 32-bit addresses and counts
 * across both ILP32 and LP64 data models.  At -O2, matching assertions fold away
 * entirely; any mismatch leaves an unresolved reference to np_witness_decode_mismatch(). */
void witness_verify_boundaries(void);
void witness_verify_boundaries(void) {
    /* 1. SceNpUserInfo2: 44 bytes */
    unsigned char user_buf[sizeof(SceNpUserInfo2)];
    memset(user_buf, 0, sizeof(user_buf));
    user_buf[0] = 'u'; user_buf[1] = 's'; user_buf[2] = 'e'; user_buf[3] = 'r';
    user_buf[36] = 0x12; user_buf[37] = 0x34; user_buf[38] = 0x56; user_buf[39] = 0x78;
    user_buf[40] = 0x9a; user_buf[41] = 0xbc; user_buf[42] = 0xde; user_buf[43] = 0xf0;

    SceNpUserInfo2 u;
    memcpy(&u, user_buf, sizeof(u));
    NP_WITNESS_CHECK((uint32_t)u.onlineName == 0x12345678U);
    NP_WITNESS_CHECK((uint32_t)u.avatarUrl == 0x9abcdef0U);

    /* 2. SceNpClansUpdatableClanInfo: 476 bytes */
    unsigned char clan_buf[sizeof(SceNpClansUpdatableClanInfo)];
    memset(clan_buf, 0, sizeof(clan_buf));
    clan_buf[0] = 0x01; clan_buf[1] = 0x02; clan_buf[2] = 0x03; clan_buf[3] = 0x04;
    clan_buf[468] = 0x22; clan_buf[469] = 0x33; clan_buf[470] = 0x44; clan_buf[471] = 0x55;
    clan_buf[472] = 0x00; clan_buf[473] = 0x00; clan_buf[474] = 0x01; clan_buf[475] = 0x00;

    SceNpClansUpdatableClanInfo c;
    memcpy(&c, clan_buf, sizeof(c));
    NP_WITNESS_CHECK(c.fields == 0x01020304U);
    NP_WITNESS_CHECK((uint32_t)c.binData1 == 0x22334455U);
    NP_WITNESS_CHECK(c.binData1Size == 0x00000100U);

    /* 3. SceNpCommerce2CommonData: 40 bytes */
    unsigned char cd_buf[sizeof(SceNpCommerce2CommonData)];
    memset(cd_buf, 0, sizeof(cd_buf));
    cd_buf[0] = 0x00; cd_buf[1] = 0x00; cd_buf[2] = 0x00; cd_buf[3] = 0x01;
    cd_buf[4] = 0x11; cd_buf[5] = 0x22; cd_buf[6] = 0x33; cd_buf[7] = 0x44;
    cd_buf[8] = 0x00; cd_buf[9] = 0x00; cd_buf[10] = 0x10; cd_buf[11] = 0x00;
    cd_buf[12] = 0x55; cd_buf[13] = 0x66; cd_buf[14] = 0x77; cd_buf[15] = 0x88;
    cd_buf[16] = 0x00; cd_buf[17] = 0x00; cd_buf[18] = 0x08; cd_buf[19] = 0x00;
    cd_buf[20] = 0x99; cd_buf[21] = 0xaa; cd_buf[22] = 0xbb; cd_buf[23] = 0xcc;
    cd_buf[24] = 0x00; cd_buf[25] = 0x00; cd_buf[26] = 0x00; cd_buf[27] = 0x05;

    SceNpCommerce2CommonData cd;
    memcpy(&cd, cd_buf, sizeof(cd));
    NP_WITNESS_CHECK(cd.version == 0x00000001U);
    NP_WITNESS_CHECK((uint32_t)cd.buf_head == 0x11223344U);
    NP_WITNESS_CHECK((uint32_t)cd.bufHead == 0x11223344U);
    NP_WITNESS_CHECK(cd.buf_size == 0x00001000U);
    NP_WITNESS_CHECK(cd.bufSize == 0x00001000U);
    NP_WITNESS_CHECK((uint32_t)cd.data == 0x55667788U);
    NP_WITNESS_CHECK(cd.data_size == 0x00000800U);
    NP_WITNESS_CHECK(cd.dataSize == 0x00000800U);
    NP_WITNESS_CHECK((uint32_t)cd.data2 == 0x99aabbccU);
    NP_WITNESS_CHECK(cd.ov == 0x00000005U);

#if defined(MUTANT_WITNESS_ALWAYS_FAIL)
    np_witness_decode_mismatch();
#endif
}

int main(void) {
    np_pointer_type_semantics_check();
    witness_verify_boundaries();
    return 0;
}
