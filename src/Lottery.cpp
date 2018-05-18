#include "Lottery.h"
#include "Discord.h"
#include "RPCManager.h"
#include <functional>
#include <fstream>
#include "cereal/archives/json.hpp"
#include "cereal/types/list.hpp"
#include "Poco/StringTokenizer.h"
#include "Poco/Thread.h"
#include "Config.h"

#define CLASS_RESOLUTION(x) std::bind(&Lottery::x, this, std::placeholders::_1, std::placeholders::_2, std::placeholders::_3)
Lottery::Lottery(TIPBOT * DP) : DiscordPtr(DP), lotterySuspended(false)
{
    Commands =
    {
        // User Commands 
        // Command                      Function                                      Params            Wallet  Admin   Allowed Channel
        { "!lottery",                   CLASS_RESOLUTION(LotteryHelp),                "",               false,  false,  AllowChannelTypes::Any        },
        { "!jackpot",                   CLASS_RESOLUTION(Jackpot),                    "",               false,  false,  AllowChannelTypes::Any        },
        { "!gameinfo",                  CLASS_RESOLUTION(gameInfo),                   "",               false,  false,  AllowChannelTypes::Any        },
        { "!mytickets",                 CLASS_RESOLUTION(MyTickets),                  "",               false,  false,  AllowChannelTypes::Any        },
        { "!buytickets",                CLASS_RESOLUTION(BuyTicket),                  "[amount]",       true,   false,  AllowChannelTypes::Any        },
        { "!togglelotterysuspend",      CLASS_RESOLUTION(ToggleLotterySuspend),       "",               false,  true,   AllowChannelTypes::Private    },
    };
    LotteryAccount = RPCManager::manuallyCreateRPC(LOTTERY_USER, GlobalConfig.RPCManager.starting_port_number - 1);
}

Lottery::~Lottery()
{
    try 
    {
        LotteryAccount->MyRPC.store();
        LotteryAccount->MyRPC.stopWallet();
    }
    catch (...)
    {

    }
}

void Lottery::save()
{
    std::ofstream out(LOTTERY_SAVE_FILE, std::ios::trunc);
    if (out.is_open())
    {
        std::cout << "Saving lottery data to disk...\n";
        {
            cereal::JSONOutputArchive ar(out);
            ar(CEREAL_NVP(lastWinningTopBlock));
        }
        out.close();
    }
}

void Lottery::load()
{
    std::ifstream in(LOTTERY_SAVE_FILE);
    if (in.is_open())
    {
        std::cout << "Loading lottery data from the disk...\n";
        {
            cereal::JSONInputArchive ar(in);
            ar(CEREAL_NVP(lastWinningTopBlock));
        }
        in.close();
    }

    // Create lottery thread
    std::thread t1(&Lottery::run, this);
    t1.detach();
}

void Lottery::setAccount(Account* acc)
{
    currentUsrAccount = acc;
}

iterator Lottery::begin()
{
    return Commands.begin();
}

const_iterator Lottery::begin() const
{
    return Commands.begin();
}

const_iterator Lottery::cbegin() const
{
    return Commands.cbegin();
}

iterator Lottery::end()
{
    return Commands.end();
}

const_iterator Lottery::end() const
{
    return Commands.end();
}

const_iterator Lottery::cend() const
{
    return Commands.cend();
}

void Lottery::run()
{
    bool rewardGivenout = false;
    bool sweepComplete = false;
    bool noWinner = false;

    GlobalConfig.General.Threads++;

    while (!GlobalConfig.General.Shutdown)
    {
        if (!lotterySuspended)
        {
            Poco::DateTime curr;
            if (!noWinner && !rewardGivenout && curr.dayOfWeek() == GlobalConfig.Lottery.day && curr.hour() == GlobalConfig.Lottery.pick)
            {
                std::cout << "Choosing Winners\n";
                try
                {
                    LotteryAccount->MyAccount.resyncAccount();

                    // Calcualte jackpot.
                    std::vector<DiscordID> enteries;
                    auto txs = LotteryAccount->MyRPC.getTransfers();
                    if (!txs.tx_in.empty())
                    {
                        std::uint64_t bal = 0;
                        unsigned int tickets;

                        // Add tickets to entry list
                        for (auto tx : txs.tx_in)
                        {
                            if (tx.block_height > lastWinningTopBlock)
                            {
                                tickets = (tx.amount / GlobalConfig.RPC.coin_offset) / GlobalConfig.Lottery.ticket_cost;
                                for (int i = 0; i < tickets; i++)
                                    enteries.emplace_back(tx.payment_id);
                                bal += tx.amount;
                            }
                        }

                        if (!enteries.empty())
                        {
                            // Add 20% empty tickets.
                            const auto amountOfBlankTickets = enteries.size() * GlobalConfig.Lottery.no_winner_chance;
                            for (auto i = 0; i < amountOfBlankTickets; i++)
                                enteries.emplace_back(0);

                            // Randomly shuffle list.
                            std::shuffle(enteries.begin(), enteries.end(), std::mt19937(std::random_device()()));

                            DiscordID winner = *enteries.begin();

                            if (winner)
                            {
                                std::cout << "The winner is " << winner << "\n";
                                lastWinningTopBlock = txs.tx_in.begin()->block_height;
                                const std::uint64_t reward = bal - (bal * GlobalConfig.Lottery.donation_percent);
                                auto WinnerAccount = RPCMan->getAccount(winner);
                                DiscordPtr->sendMessage(DiscordPtr->getDiscordDMChannel(winner), Poco::format("You've won %0.8f %s from the lottery! :money_with_wings:", reward / GlobalConfig.RPC.coin_offset, GlobalConfig.RPC.coin_abbv));
                                LotteryAccount->MyAccount.transferMoneyToAddress(reward, WinnerAccount.getMyAddress());
                            }
                            else
                            {
                                std::cout << "No Winner!\n";
                                noWinner = true;
                            }
                            DiscordPtr->AppSave();
                            rewardGivenout = true;
                        } std::cerr << "No Active Tickets!\n";
                    }
                    else std::cerr << "Error transaction list is empty!\n";
                }
                catch (...)
                {
                    lotterySuspended = true;
                }
            }
            else if (!sweepComplete && curr.dayOfWeek() == GlobalConfig.Lottery.day && curr.hour() == GlobalConfig.Lottery.faucet)
            {
                try
                {
                    LotteryAccount->MyAccount.resyncAccount();

                    // Donate Remaining to faucet.
                    if (!noWinner)
                        LotteryAccount->MyAccount.transferAllMoneyToAddress(RPCManager::getGlobalBotAccount().getMyAddress());

                    noWinner = false;
                    sweepComplete = true;
                } catch (...)
                {
                    Poco::Thread::sleep(29000);
                }
            }
            else
            {
                if ((rewardGivenout && sweepComplete) || (rewardGivenout && noWinner && curr.hour() < GlobalConfig.Lottery.close))
                {
                    sweepComplete = false;
                    rewardGivenout = false;
                }
            }
        }
        Poco::Thread::sleep(1000);
    }

    GlobalConfig.General.Threads--;
}

void Lottery::gameInfo(TIPBOT* DiscordPtr, const SleepyDiscord::Message& message, const Command& me) const
{
    std::stringstream ss;

    ss << "Game Info:\\n";
    ss << "```";
    ss << "Minimum Ticket Cost " << GlobalConfig.Lottery.ticket_cost << " " << GlobalConfig.RPC.coin_abbv << "\\n";
    ss << "Faucet Donation: " << GlobalConfig.Lottery.donation_percent * 100 << "% of the reward\\n";
    ss << "No Winner: " << GlobalConfig.Lottery.no_winner_chance * 100 << "% of the drawing will be no winner.\\n";
    ss << "Days: Lottery starts on Saturday 12 AM UTC and end on Friday 6 PM UTC. Winners announced on Friday 9 PM UTC\\n";
    ss << "In the event of no winner the jackpot is rolled over to next drawing\\n";
    ss << "Winner will be direct messaged.\\n";
    ss << "```";
    DiscordPtr->sendMessage(message.channelID, ss.str());
}

void Lottery::LotteryHelp(TIPBOT* DiscordPtr, const SleepyDiscord::Message& message, const Command& me) const
{
    const auto channelType = DiscordPtr->getDiscordChannelType(message.channelID);
    const auto helpStr = TIPBOT::generateHelpText("Lottery Commands:\\n", Commands, channelType, message);
    DiscordPtr->sendMessage(message.channelID, helpStr);
}

void Lottery::Jackpot(TIPBOT* DiscordPtr, const SleepyDiscord::Message& message, const Command& me) const
{
    // Calcualte jackpot.
    std::uint64_t bal = 0;
    auto txs = LotteryAccount->MyRPC.getTransfers();
    for (auto tx : txs.tx_in)
    {
        if (tx.block_height > lastWinningTopBlock)
        {
            bal += tx.amount;
        }
    }
    DiscordPtr->sendMessage(message.channelID, Poco::format("The current jackpot is: %0.8f", bal / GlobalConfig.RPC.coin_offset));
}

void Lottery::BuyTicket(TIPBOT* DiscordPtr, const SleepyDiscord::Message& message, const Command& me) const
{
    Poco::DateTime curr;
    if (curr.dayOfWeek() != GlobalConfig.Lottery.day || (curr.dayOfWeek() == GlobalConfig.Lottery.day && curr.hour() < GlobalConfig.Lottery.close))
    {
        if (!lotterySuspended)
        {
            Poco::StringTokenizer cmd(message.content, " ");

            if (cmd.count() != 2)
                DiscordPtr->CommandParseError(message, me);
            else
            {
                LotteryAccount->MyAccount.resyncAccount();
                const auto tickets = Poco::NumberParser::parseUnsigned(cmd[1]);
                const auto tx = currentUsrAccount->transferMoneyToAddress((tickets * GlobalConfig.Lottery.ticket_cost) * GlobalConfig.RPC.coin_offset, LotteryAccount->MyAccount.getMyAddress());
                DiscordPtr->sendMessage(message.channelID, Poco::format("%s#%s: Purchased %?i tickets for %0.8f %s with TX Hash: %s :smiley:", message.author.username, message.author.discriminator, tickets, tickets * GlobalConfig.Lottery.ticket_cost, GlobalConfig.RPC.coin_abbv, tx.tx_hash));
            }
        }
        else DiscordPtr->sendMessage(message.channelID, "Lottery is currently suspended.");
    }
    else DiscordPtr->sendMessage(message.channelID, "Lottery is closed until 12 AM UTC.");
}

void Lottery::MyTickets(TIPBOT* DiscordPtr, const SleepyDiscord::Message& message, const Command& me) const
{
    LotteryAccount->MyAccount.resyncAccount();

    // Calcualte jackpot.
    std::uint64_t bal = 0;
    auto txs = LotteryAccount->MyRPC.getTransfers();
    for (auto tx : txs.tx_in)
    {
        if (tx.block_height > lastWinningTopBlock && tx.payment_id == TIPBOT::convertSnowflakeToInt64(message.author.ID))
        {
            bal += tx.amount;
        }
    }
    DiscordPtr->sendMessage(message.channelID, Poco::format("You currently have %Lu active tickets.", static_cast<uint64_t>((bal / GlobalConfig.RPC.coin_offset) / GlobalConfig.Lottery.ticket_cost)));
}

void Lottery::ToggleLotterySuspend(TIPBOT* DiscordPtr, const SleepyDiscord::Message& message, const Command& me)
{
    lotterySuspended = !lotterySuspended;    
    DiscordPtr->AppSave();
    DiscordPtr->sendMessage(message.channelID, Poco::format("Lottery Suspended: %b", lotterySuspended));
}
