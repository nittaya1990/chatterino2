#include "common/CompletionModel.hpp"

#include "Application.hpp"
#include "common/ChatterSet.hpp"
#include "common/Common.hpp"
#include "controllers/accounts/AccountController.hpp"
#include "controllers/commands/CommandController.hpp"
#include "debug/Benchmark.hpp"
#include "providers/twitch/TwitchChannel.hpp"
#include "providers/twitch/TwitchIrcServer.hpp"
#include "singletons/Emotes.hpp"
#include "singletons/Settings.hpp"
#include "util/Helpers.hpp"
#include "util/QStringHash.hpp"

#include <QtAlgorithms>
#include <utility>

namespace chatterino {

//
// TaggedString
//

CompletionModel::TaggedString::TaggedString(const QString &_string, Type _type)
    : string(_string)
    , type(_type)
{
}

bool CompletionModel::TaggedString::isEmote() const
{
    return this->type > Type::EmoteStart && this->type < Type::EmoteEnd;
}

bool CompletionModel::TaggedString::operator<(const TaggedString &that) const
{
    if (this->isEmote() != that.isEmote())
    {
        return this->isEmote();
    }

    return CompletionModel::compareStrings(this->string, that.string);
}

//
// CompletionModel
//
CompletionModel::CompletionModel(Channel &channel)
    : channel_(channel)
{
}

int CompletionModel::columnCount(const QModelIndex &) const
{
    return 1;
}

QVariant CompletionModel::data(const QModelIndex &index, int) const
{
    std::lock_guard<std::mutex> lock(this->itemsMutex_);

    auto it = this->items_.begin();
    std::advance(it, index.row());
    return QVariant(it->string);
}

int CompletionModel::rowCount(const QModelIndex &) const
{
    std::lock_guard<std::mutex> lock(this->itemsMutex_);

    return this->items_.size();
}

void CompletionModel::refresh(const QString &prefix, bool isFirstWord)
{
    std::lock_guard<std::mutex> guard(this->itemsMutex_);
    this->items_.clear();

    if (prefix.length() < 2 || !this->channel_.isTwitchChannel())
    {
        return;
    }

    // Twitch channel
    auto tc = dynamic_cast<TwitchChannel *>(&this->channel_);

    std::function<void(const QString &str, TaggedString::Type type)> addString;
    if (getSettings()->prefixOnlyEmoteCompletion)
    {
        addString = [=](const QString &str, TaggedString::Type type) {
            if (str.startsWith(prefix, Qt::CaseInsensitive))
                this->items_.emplace(str + " ", type);
        };
    }
    else
    {
        addString = [=](const QString &str, TaggedString::Type type) {
            if (str.contains(prefix, Qt::CaseInsensitive))
                this->items_.emplace(str + " ", type);
        };
    }

    if (auto account = getApp()->accounts->twitch.getCurrent())
    {
        // Twitch Emotes available globally
        for (const auto &emote : account->accessEmotes()->emotes)
        {
            addString(emote.first.string, TaggedString::TwitchGlobalEmote);
        }

        // Twitch Emotes available locally
        auto localEmoteData = account->accessLocalEmotes();
        if (tc && localEmoteData->find(tc->roomId()) != localEmoteData->end())
        {
            for (const auto &emote : localEmoteData->at(tc->roomId()))
            {
                addString(emote.first.string,
                          TaggedString::Type::TwitchLocalEmote);
            }
        }
    }

    // Bttv Global
    for (auto &emote : *getApp()->twitch2->getBttvEmotes().emotes())
    {
        addString(emote.first.string, TaggedString::Type::BTTVChannelEmote);
    }

    // Ffz Global
    for (auto &emote : *getApp()->twitch2->getFfzEmotes().emotes())
    {
        addString(emote.first.string, TaggedString::Type::FFZChannelEmote);
    }

    // Emojis
    if (prefix.startsWith(":"))
    {
        const auto &emojiShortCodes = getApp()->emotes->emojis.shortCodes;
        for (auto &m : emojiShortCodes)
        {
            addString(QString(":%1:").arg(m), TaggedString::Type::Emoji);
        }
    }

    //
    // Stuff below is available only in regular Twitch channels
    if (!tc)
    {
        return;
    }

    // Usernames
    if (prefix.startsWith("@"))
    {
        QString usernamePrefix = prefix;
        usernamePrefix.remove(0, 1);

        auto chatters = tc->accessChatters()->filterByPrefix(usernamePrefix);

        for (const auto &name : chatters)
        {
            addString(
                "@" + formatUserMention(name, isFirstWord,
                                        getSettings()->mentionUsersWithComma),
                TaggedString::Type::Username);
        }
    }
    else if (!getSettings()->userCompletionOnlyWithAt)
    {
        auto chatters = tc->accessChatters()->filterByPrefix(prefix);

        for (const auto &name : chatters)
        {
            addString(formatUserMention(name, isFirstWord,
                                        getSettings()->mentionUsersWithComma),
                      TaggedString::Type::Username);
        }
    }

    // Bttv Channel
    for (auto &emote : *tc->bttvEmotes())
    {
        addString(emote.first.string, TaggedString::Type::BTTVGlobalEmote);
    }

    // Ffz Channel
    for (auto &emote : *tc->ffzEmotes())
    {
        addString(emote.first.string, TaggedString::Type::BTTVGlobalEmote);
    }

    // Commands
    for (auto &command : getApp()->commands->items_)
    {
        addString(command.name, TaggedString::Command);
    }

    for (auto &command : getApp()->commands->getDefaultTwitchCommandList())
    {
        addString(command, TaggedString::Command);
    }
}

bool CompletionModel::compareStrings(const QString &a, const QString &b)
{
    // try comparing insensitively, if they are the same then senstively
    // (fixes order of LuL and LUL)
    int k = QString::compare(a, b, Qt::CaseInsensitive);
    if (k == 0)
        return a > b;

    return k < 0;
}

}  // namespace chatterino
