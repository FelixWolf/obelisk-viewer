#include "InstantMessages.hpp"
#include "Offers.hpp"

#include "viewer/protocol/Messages.hpp"

#include <doctest/doctest.h>

#include <string>

using namespace viewer;
namespace msg = viewer::protocol::msg;

namespace
{

const Uuid kSelf = *Uuid::parse("11111111-1111-1111-1111-111111111111");
const Uuid kAnn = *Uuid::parse("22222222-2222-2222-2222-222222222222");
const Uuid kGroup = *Uuid::parse("6a1b2c3d-0000-4000-8000-00000000abcd");
const Uuid kItem = *Uuid::parse("44444444-4444-4444-4444-444444444444");
const Uuid kTransaction = *Uuid::parse("55555555-5555-5555-5555-555555555555");

msg::ImprovedInstantMessage im(uint8_t dialog, std::string text, std::vector<uint8_t> bucket = {0},
                               const Uuid& from = kAnn, std::string name = "Ann Resident")
{
    msg::ImprovedInstantMessage m;
    m.AgentData.AgentID = from;
    m.MessageBlock.ToAgentID = kSelf;
    m.MessageBlock.Dialog = dialog;
    m.MessageBlock.ID = kTransaction;
    m.MessageBlock.FromAgentName = protocol::fromString(name);
    m.MessageBlock.Message = protocol::fromString(text);
    m.MessageBlock.BinaryBucket = std::move(bucket);
    return m;
}

std::vector<uint8_t> text(std::string_view s, bool terminated = true)
{
    std::vector<uint8_t> out(s.begin(), s.end());
    if(terminated)
    {
        out.push_back(0);
    }
    return out;
}

} // namespace

TEST_CASE("Offers: a teleport offered, taken or declined")
{
    Offers offers;
    CHECK(offers.handle(
        im(ImDialog::LureUser, "Join me in Mauve", text("256000|256000|128|128|30|0|1|0|M")), 7));
    REQUIRE(offers.front());
    CHECK(offers.front()->kind == Offer::Kind::Teleport);
    CHECK(offers.front()->fromName == "Ann");
    CHECK(offers.front()->rating == "Moderate");
    CHECK(offers.front()->region == 7);
    // A newer one from Ann replaces it.
    offers.handle(im(ImDialog::LureUser, "Here instead", text("")), 7);
    CHECK(offers.waiting() == 1);
    CHECK(offers.front()->message == "Here instead");

    offers.accept();
    CHECK_FALSE(offers.front());
    auto answers = offers.takeAnswers();
    REQUIRE(answers.size() == 1);
    REQUIRE(answers[0].lure);
    CHECK(*answers[0].lure == kTransaction);
    CHECK_FALSE(answers[0].godlike);

    offers.handle(im(ImDialog::LureUser, "Again", text("")), 7);
    offers.decline();
    answers = offers.takeAnswers();
    REQUIRE(answers.size() == 1);
    REQUIRE(answers[0].im);
    CHECK(answers[0].im->to == kAnn);
    CHECK(answers[0].im->id == kTransaction);
    CHECK(answers[0].im->dialog == ImDialog::LureDeclined);
    CHECK_FALSE(answers[0].im->folderType);

    // A summons: taken at once.
    offers.handle(im(ImDialog::GodlikeLureUser, "", text("")), 7);
    CHECK_FALSE(offers.front());
    answers = offers.takeAnswers();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].godlike);
}

TEST_CASE("Offers: inventory from someone is already ours; from an object, kept or declined")
{
    Offers offers;
    std::vector<uint8_t> bucket{7}; // a notecard
    bucket.insert(bucket.end(), kItem.bytes.begin(), kItem.bytes.end());
    offers.handle(im(ImDialog::InventoryOffered, "Welcome notecard", bucket), 0);
    // Accepted into the notecards folder as it comes.
    auto answers = offers.takeAnswers();
    REQUIRE(answers.size() == 1);
    REQUIRE(answers[0].im);
    CHECK(answers[0].im->dialog == ImDialog::InventoryAccepted);
    CHECK(answers[0].im->folderType == 7);
    REQUIRE(offers.front());
    CHECK(offers.front()->item == kItem);
    CHECK(offers.front()->itemName == "Welcome notecard");
    offers.decline(); // discarded: to the Trash
    answers = offers.takeAnswers();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].trash == kItem);

    offers.handle(im(ImDialog::TaskInventoryOffered, "Free shirt", {5}, kAnn, "Vendor"), 0);
    REQUIRE(offers.front());
    CHECK(offers.front()->kind == Offer::Kind::ObjectInventory);
    CHECK(offers.front()->fromObject);
    CHECK(offers.front()->fromName == "Vendor");
    CHECK(offers.front()->assetType == 5);
    CHECK(offers.takeAnswers().empty()); // not until asked
    offers.accept();
    answers = offers.takeAnswers();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].im->dialog == ImDialog::TaskInventoryAccepted);
    CHECK(answers[0].im->folderType == 5);
    CHECK(answers[0].im->to == kAnn);

    // Older regions' bucket: "type|...".
    offers.handle(im(ImDialog::TaskInventoryOffered, "Box", text("6|0")), 0);
    CHECK(offers.front()->assetType == 6);
    offers.decline();
    answers = offers.takeAnswers();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].im->dialog == ImDialog::TaskInventoryDeclined);
}

TEST_CASE("Offers: friendship and group invitations")
{
    Offers offers;
    offers.handle(im(ImDialog::FriendshipOffered, "Be my friend?"), 0);
    offers.handle(im(ImDialog::FriendshipOffered, "Be my friend?"), 0);
    CHECK(offers.waiting() == 1); // once from someone
    offers.accept();
    auto answers = offers.takeAnswers();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].friendship == true);
    CHECK(answers[0].transaction == kTransaction);

    // Offered while away: by capability (nothing sent as a message).
    auto away = im(ImDialog::FriendshipOffered, "Later?");
    away.MessageBlock.Offline = 1;
    offers.handle(away, 0);
    offers.decline();
    CHECK(offers.takeAnswers().empty());

    // A group invitation: the fee (big-endian) and the role in the bucket.
    std::vector<uint8_t> bucket{0, 0, 0x01, 0x2c}; // L$300
    bucket.resize(20, 0);
    auto invite = im(ImDialog::GroupInvitation, "Join Builders!", bucket, kGroup, "Ann Resident");
    invite.MessageBlock.FromGroup = true;
    offers.handle(invite, 0);
    REQUIRE(offers.front());
    CHECK(offers.front()->kind == Offer::Kind::GroupInvite);
    CHECK(offers.front()->fee == 300);
    CHECK(offers.front()->group == kGroup);
    offers.accept();
    answers = offers.takeAnswers();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].im->to == kGroup);
    CHECK(answers[0].im->dialog == ImDialog::GroupInvitationAccept);
    CHECK_FALSE(answers[0].im->folderType);

    // A wrong-sized bucket: nothing shown.
    offers.handle(im(ImDialog::GroupInvitation, "?", {0, 0}), 0);
    CHECK_FALSE(offers.front());
}

TEST_CASE("Offers: group notices, with and without an attachment")
{
    Offers offers;
    // The bucket: attached, asset type, the group, the item's name.
    std::vector<uint8_t> bucket{1, 7};
    bucket.insert(bucket.end(), kGroup.bytes.begin(), kGroup.bytes.end());
    const auto name = text("Rules");
    bucket.insert(bucket.end(), name.begin(), name.end());
    offers.handle(im(ImDialog::GroupNotice, "Meeting|Friday at 8", bucket, kGroup, "Ann Resident"),
                  0);
    REQUIRE(offers.front());
    const Offer& o = *offers.front();
    CHECK(o.kind == Offer::Kind::GroupNotice);
    CHECK(o.subject == "Meeting");
    CHECK(o.message == "Friday at 8");
    CHECK(o.group == kGroup);
    CHECK(o.assetType == 7);
    CHECK(o.itemName == "Rules");
    offers.accept();
    auto answers = offers.takeAnswers();
    REQUIRE(answers.size() == 1);
    CHECK(answers[0].im->dialog == ImDialog::GroupNoticeInventoryAccepted);
    CHECK(answers[0].im->folderType == 7);

    // The group in RegionID, the bucket "type|wearable|name"; and none
    // attached: closing it says nothing.
    auto plain = im(ImDialog::GroupNotice, "Hello|No attachment", {0});
    plain.MessageBlock.RegionID = kGroup;
    offers.handle(plain, 0);
    REQUIRE(offers.front());
    CHECK(offers.front()->assetType == -1);
    CHECK(offers.front()->group == kGroup);
    offers.decline();
    CHECK(offers.takeAnswers().empty());
}

TEST_CASE("Offers: not offers, and what came while away")
{
    Offers offers;
    CHECK_FALSE(offers.handle(im(ImDialog::NothingSpecial, "hi"), 0));
    CHECK_FALSE(offers.handle(im(ImDialog::TypingStart, ""), 0));
    CHECK(offers.handle(im(ImDialog::InventoryAccepted, ""), 0)); // taken, nothing shown
    CHECK_FALSE(offers.front());

    llsd::Value record;
    record["from_agent_id"] = kAnn;
    record["from_agent_name"] = "Ann Resident";
    record["dialog"] = static_cast<int32_t>(ImDialog::FriendshipOffered);
    record["message"] = "Friends?";
    record["from_group"] = "N";
    const msg::ImprovedInstantMessage m = offlineMessage(record);
    CHECK(m.MessageBlock.Offline == 1);
    CHECK_FALSE(m.MessageBlock.FromGroup);
    CHECK(m.AgentData.AgentID == kAnn);
    CHECK(offers.handle(m, 0));
    REQUIRE(offers.front());
    CHECK(offers.front()->offline);
}
