#include "cptblackbill.hpp"

using namespace eosio;

class [[eosio::contract]] cptblackbill : public eosio::contract {

public:
    using contract::contract;
    
    cptblackbill(name receiver, name code,  datastream<const char*> ds): contract(receiver, code, ds) {}
    
    //Issue token
    [[eosio::action]]
    void issue(name to, asset quantity, std::string memo )
    {
        auto sym = quantity.symbol;
        eosio_assert( sym.is_valid(), "invalid symbol name" );
        eosio_assert( memo.size() <= 256, "memo has more than 256 bytes" );

        stats statstable( _self, sym.code().raw() );
        auto existing = statstable.find( sym.code().raw() );
        eosio_assert( existing != statstable.end(), "token with symbol does not exist, create token before issue" );
        const auto& st = *existing;

        require_auth( st.issuer );
        eosio_assert( quantity.is_valid(), "invalid quantity" );
        eosio_assert( quantity.amount > 0, "must issue positive quantity" );

        eosio_assert( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
        eosio_assert( quantity.amount <= st.max_supply.amount - st.supply.amount, "quantity exceeds available supply");

        statstable.modify( st, same_payer, [&]( auto& s ) {
            s.supply += quantity;
        });

        add_balance( st.issuer, quantity, st.issuer );

        if( to != st.issuer ) {
            SEND_INLINE_ACTION( *this, transfer, { {st.issuer, "active"_n} }, { st.issuer, to, quantity, memo });
        }
    }

    //Transfer token
    [[eosio::action]]
    void transfer(name from, name to, asset quantity, std::string memo )
    {
        eosio_assert( from != to, "cannot transfer to self" );
        require_auth( from );
        eosio_assert( is_account( to ), "to account does not exist");
        auto sym = quantity.symbol.code();
        stats statstable( _self, sym.raw() );
        const auto& st = statstable.get( sym.raw() );

        require_recipient( from );
        require_recipient( to );

        eosio_assert( quantity.is_valid(), "invalid quantity" );
        eosio_assert( quantity.amount > 0, "must transfer positive quantity" );
        eosio_assert( quantity.symbol == st.supply.symbol, "symbol precision mismatch" );
        eosio_assert( memo.size() <= 256, "memo has more than 256 bytes" );

        auto payer = has_auth( to ) ? to : from;

        sub_balance( from, quantity );
        add_balance( to, quantity, payer );

        //2020-05-16 If sent to cptblackbill then add quantity to sell order on exchngtokens (selling BLKBILL tokens)
        if (to == name{"cptblackbill"} && quantity.symbol == symbol(symbol_code("BLKBILL"), 4))
        {
            exchngtokens_index exchngtokens(_self, _self.value);
            uint64_t itemPriceInDollar = std::strtoull(memo.c_str(), NULL, 0);
            itemPriceInDollar = itemPriceInDollar * 100; //memo is sell amount in cent 

            exchngtokens.emplace(_self, [&]( auto& row ) {
                row.pkey = exchngtokens.available_primary_key();
                row.account = from;
                row.sell = quantity; 
                row.itemprice = eosio::asset(itemPriceInDollar, symbol(symbol_code("USD"), 4));
                row.timestamp = now();
            });
        }
        //----------------------
    }

    static asset get_balance(name token_contract_account, name owner, symbol_code sym_code) {
        accounts accountstable(token_contract_account, owner.value);
        const auto& ac = accountstable.get(sym_code.raw());
        return ac.balance;
    }

    void sub_balance(name owner, asset value) 
    {
        accounts from_acnts( _self, owner.value );

        const auto& from = from_acnts.get( value.symbol.code().raw(), "no balance object found" );
        eosio_assert( from.balance.amount >= value.amount, "overdrawn balance" );

        from_acnts.modify( from, owner, [&]( auto& a ) {
            a.balance -= value;
        });
    }

    void add_balance( name owner, asset value, name ram_payer )
    {
        accounts to_acnts( _self, owner.value );
        auto to = to_acnts.find( value.symbol.code().raw() );
        if( to == to_acnts.end() ) {
            to_acnts.emplace( ram_payer, [&]( auto& a ){
                a.balance = value;
            });
        } else {
            to_acnts.modify( to, same_payer, [&]( auto& a ) {
                a.balance += value;
            });
        }
    }

    //===Receive EOS token=================================================
    void onTransfer(name from, name to, asset eos, std::string memo) { 
        // verify that this is an incoming transfer
        if (to != name{"cptblackbill"})
            return;

        eosio_assert(eos.symbol == symbol(symbol_code("EOS"), 4), "must pay with EOS token");
        eosio_assert(eos.amount > 0, "deposit amount must be positive");

        if (memo.rfind("Check Treasure No.", 0) == 0) {
            //from account pays to check a treasure value

            eosio_assert(eos >= getPriceForCheckTreasureValueInEOS(), "Transfered amount is below minimum price for checking treasure value. Please refresh webpage.");
            
            uint64_t treasurepkey = std::strtoull( memo.substr(18).c_str(),NULL,0 ); //Find treasure pkey from transfer memo
            
            treasure_index treasures(_self, _self.value);
            auto iterator = treasures.find(treasurepkey);
            eosio_assert(iterator != treasures.end(), "Treasure not found.");
            eosio_assert(iterator->status == "active", "Treasure is not active.");
            
            //eosio::asset totcrfund = (eos * (25 * 100)) / 10000; //25 percent provision to diamond owners
            eosio::asset toLostDiamondValueByCptBlackBill = (eos * (90 * 100)) / 10000; //90 percent to diamond value 

            //Update diamond ownership for cptblackbill
            //The provision earned to account cptblackbill is transfered to a random treasure when the lost diamond is found
            /*diamondownrs_index diamondownrs(_self, _self.value);
            auto account_index = diamondownrs.get_index<name("account")>();
            auto diamondownrsItr = account_index.find(to.value);
            if(diamondownrsItr == account_index.end()){
                diamondownrs.emplace(_self, [&]( auto& row ) {
                    row.pkey = diamondownrs.available_primary_key();
                    row.account = to;
                    row.investedamount = toLostDiamondValueByCptBlackBill;
                    row.investedpercent = 0; //Null by default. Will be recalculated later
                    row.earnedpayout = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                    row.timestamp = now();
                });
            }
            else{
                account_index.modify(diamondownrsItr, _self, [&]( auto& row ) {
                    row.investedamount += toLostDiamondValueByCptBlackBill;
                });            
            }*/

            //2020-02-24 Add to diamond fund
            eosio::asset toTokenHolders = (eos * (10 * 100)) / 10000;
            diamondfund_index diamondfund(_self, _self.value);
            auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
            auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
            diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                //row.toDiamondOwners += totcrfund;
                row.toTokenHolders += toTokenHolders;
                row.diamondValue += toLostDiamondValueByCptBlackBill;
            });  
        }
        else if (memo.rfind("Unlock Treasure No.", 0) == 0) {
            //from account pays to unlock a treasure

            //Get treasurepkey and secret code from memo
            uint64_t treasurepkey = std::strtoull( memo.substr(19).c_str(),NULL,0 ); //Find treasure pkey from transfer memo
            
            treasure_index treasures(_self, _self.value);
            auto iterator = treasures.find(treasurepkey);
            eosio_assert(iterator != treasures.end(), "Treasure not found.");
            eosio_assert(iterator->status == "active", "Treasure is not active.");
            eosio_assert(eos >= getPriceForCheckTreasureValueInEOS(), "Transfered amount is below minimum price for unlocking a treasure.");
                
            //Owner of the treasure can only unlock a treasure if it's conquered by someone else. And if conquered, the
            //user that has conquered can not unlock as long as that account is registered as conquered.
            if(is_account( iterator->conqueredby))
                eosio_assert(iterator->conqueredby != from, "You are not allowed to unlock a treasure you have conquered.");
            else
                eosio_assert(iterator->owner != from, "You are not allowed to unlock your own treasure.");
        
        }
        else if (memo.rfind("Wrong code payment on treasure No.", 0) == 0) {
            //from account pays to unlock a treasure

            //Get treasurepkey and secret code from memo
            uint64_t treasurepkey = std::strtoull( memo.substr(34).c_str(),NULL,0 ); //Find treasure pkey from transfer memo
            
            treasure_index treasures(_self, _self.value);
            auto iterator = treasures.find(treasurepkey);
            eosio_assert(iterator != treasures.end(), "Treasure not found.");
            eosio_assert(iterator->status == "active", "Treasure is not active.");
            eosio_assert(eos >= getPriceForCheckTreasureValueInEOS(), "Transfered amount is below minimum price for unlocking a treasure.");
            
            //2020-02-24 Add to diamond fund
            eosio::asset toTokenHolders = (eos * (10 * 100)) / 10000;
            eosio::asset toLostDiamondValueByCptBlackBill = (eos * (90 * 100)) / 10000;
            diamondfund_index diamondfund(_self, _self.value);
            auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
            auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
            diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                row.toTokenHolders += toTokenHolders;
                row.diamondValue += toLostDiamondValueByCptBlackBill;
            });
            
        }
        else if (memo.rfind("Activate SponsorItem No.", 0) == 0) {
            uint64_t sponsorItemPkey = std::strtoull( memo.substr(24).c_str(),NULL,0 ); //Find treasure pkey from transfer memo

            sponsoritems_index sponsoritems(_self, _self.value);
            auto iterator = sponsoritems.find(sponsorItemPkey);
            eosio_assert(iterator != sponsoritems.end(), "Sponsor item not found.");
            eosio_assert(iterator->status == "pendingforadfeepayment", "Sponsor item is not pending for payment.");
            eosio_assert(eos.amount >= iterator->adFeePrice.amount, "Payment amount is less than advertising fee.");

            ////Take percent of the transfered EOS as provision to the lost diamond owners
            eosio::asset totcrfund = (eos * (20 * 100)) / 10000;
            
            //2020-02-25: This will replace the code above
            //Add 10 percent to the value of the Lost Diamond (account cptblackbill is used for this)
            /*diamondownrs_index diamondownrs(_self, _self.value);
            auto account_index = diamondownrs.get_index<name("account")>();
            auto diamondownrsItr = account_index.find(to.value);
            if(diamondownrsItr == account_index.end()){
                diamondownrs.emplace(_self, [&]( auto& row ) {
                    row.pkey = diamondownrs.available_primary_key();
                    row.account = to;
                    row.investedamount = totcrfund;
                    row.investedpercent = 0; //Null by default. Will be recalculated later
                    row.earnedpayout = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                    row.timestamp = now();
                });
            }
            else{
                account_index.modify(diamondownrsItr, _self, [&]( auto& row ) {
                    row.investedamount += totcrfund;
                });            
            }*/


            //2020-02-24 Add to diamond fund
            //2021-04-25 Add to diamond value and token holders
            eosio::asset toTokenHolders = (eos * (10 * 100)) / 10000;
            diamondfund_index diamondfund(_self, _self.value);
            auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
            auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
            diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                //row.toDiamondOwners += totcrfund; //10%
                row.toTokenHolders += toTokenHolders; //10%
                row.diamondValue += totcrfund; //20%
            });  

            //The other 70% is added to the treasure value 
            sponsoritems.modify(iterator, _self, [&]( auto& row ) {
                row.treasurepkey = 0;
                row.status = "active";
            }); 
        }
        else if (memo.rfind("AddAdventureRace:", 0) == 0) { //2020-08-11
            
            //Check that amount is above minimum fee for adding a new adventure race
            eosio_assert(eos >= getPriceForCheckTreasureValueInEOS() * 2, "Transfered amount is below minimum price for creating a new adventure race.");
            
            std::string raceTitle = memo.substr(17).c_str(); //Get race title    
            
            race_index race(_self, _self.value);
            race.emplace(_self, [&]( auto& row ) {
                row.pkey = race.available_primary_key();
                row.raceowner = from;
                row.title = raceTitle;
                row.entryfeeusd = eosio::asset(0, symbol(symbol_code("USD"), 4));
                row.timestamp = now();
            });

            //Divide fee for adding new adventure race to token holders and diamond value
            eosio::asset toTokenHolders = (eos * (50 * 100)) / 10000; //50 percent to BLKBILL token holders
            eosio::asset toDiamondValue = (eos * (50 * 100)) / 10000; //50 percent to diamond value
            diamondfund_index diamondfund(_self, _self.value);
            auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
            auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
            diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                row.toTokenHolders += toTokenHolders; //50%
                row.diamondValue += toDiamondValue; //50%
            });

        }
        else if (memo.rfind("RacePayment:", 0) == 0) { //2020-08-11
            
            uint64_t racePkey = std::strtoull( memo.substr(12).c_str(),NULL,0 ); //Find race pkey for payment
            asset eosusd = getEosUsdPrice();
            double dblEosUsdPrice = eosusd.amount;

            if(racePkey == 10){
                //The Lost Diamond Entry fee
                eosio::asset toTokenHolders = (eos * (20 * 100)) / 10000; //20 percent to BLKBILL token holders
                eosio::asset toDiamondValue = (eos * (80 * 100)) / 10000; //80 percent to diamond value
                diamondfund_index diamondfund(_self, _self.value);
                auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
                auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
                diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                    row.toTokenHolders += toTokenHolders; //20%
                    row.diamondValue += toDiamondValue; //80%
                });
            }
            else{
                racepayments_index racepayments(_self, _self.value);
                racepayments.emplace(_self, [&]( auto& row ) {
                    row.pkey = racepayments.available_primary_key();
                    row.racepkey = racePkey;
                    row.teamaccount = from; //The account who sent money
                    row.entryfee = eos;
                    row.feereleased = 0; //Payed, but not sent to race owner (until race is proven to take place with solved checkpoints).
                    row.eosusdprice = eosusd;
                    row.timestamp = now();
                });
            }
            //Race payment is stored in the table racepayments until the race starts. When the
            //participants solve checkpoints, the racepayment is transfered/distrubuted to race 
            //owner, the lost diamond and token holders. This is to insure that the race takes place.
            //If the race is cancelled or for other reasons don't happen, the race payment is 
            //sent back to the participants.
            //This code is implemented in the function 'addracerslt'

        }
        else if(memo.rfind("Buy Treasure No.",0) == 0){ //2021-10-03
            
            uint64_t treasurepkey = std::strtoull( memo.substr(16).c_str(),NULL,0 ); //Find treasurePkey to buy
            
            treasure_index treasures(_self, _self.value);
            auto treasureIterator = treasures.find(treasurepkey);
            eosio_assert(treasureIterator != treasures.end(), "Treasure not found..");
            
            treasuresale_index treasuresales(_self, _self.value);
            auto idxTreasureSales = treasuresales.get_index<name("treasurepkey")>();
            auto treasuresaleIterator = idxTreasureSales.find(treasurepkey); // treasuresales.find(treasurepkey);
            eosio_assert(treasuresaleIterator != idxTreasureSales.end(), "Not for sale. Asking price for this treasure is not found.");
            
            eosio_assert(from != treasureIterator->owner, "You can not buy your own treasure.");

            name payToTreasureOwner = treasureIterator->owner;             
            asset sendAmountInUsd = getPriceInUSD(eos);
            asset eosusd = getEosUsdPrice();
            double dblAskingPriceInEOS = (treasuresaleIterator->askingpriceUsd.amount * 10000) / eosusd.amount;
            uint64_t uintAskingPriceInEOS = (uint64_t)dblAskingPriceInEOS;   
            asset askingPriceInEOS = eosio::asset(uintAskingPriceInEOS, symbol(symbol_code("EOS"), 4));       

            double dblVisibleAskPriceInEos = ((double)askingPriceInEOS.amount / 10000) + 0.0001; //Add 0.0001 for rounding issues 
            double dblVisibleAskPriceInUsd = (double)treasuresaleIterator->askingpriceUsd.amount / 10000; 
            double dblVisibleSendAmountInUsd = ((double)sendAmountInUsd.amount / 10000) + 0.0001; //Add 0.0001 for rounding issues 
            std::string assertErrorAmountToLowMsg = "Amount is to low. Asking price for this treasure is USD " + std::to_string(dblVisibleAskPriceInUsd) + " (" + std::to_string(dblVisibleAskPriceInEos) + " EOS). SendAmountInUsd: " + std::to_string(dblVisibleSendAmountInUsd);
            eosio_assert(dblVisibleSendAmountInUsd >= dblVisibleAskPriceInUsd, assertErrorAmountToLowMsg.c_str());

            //std::string debugInfo = "DEBUGTEST: sellPkey: " + std::to_string(treasuresaleIterator->pkey) + " AskPriceInEOS: " + std::to_string(dblVisibleAskPriceInEos) + " AinUSD: " + std::to_string(dblVisibleAskPriceInUsd) + " SendAmountInUsd: " + std::to_string(dblVisibleSendAmountInUsd);
            //eosio_assert(1 == 0, debugInfo.c_str());

            //Change treasure owner
            treasures.modify(treasureIterator, _self, [&]( auto& row ) {
                row.owner = from;
            });
                    
            //Remove asking price in table treasure sales
            idxTreasureSales.erase(treasuresaleIterator);

            //Add 1% Transaction fee to the lost diamond and 1% fee to token holders
            eosio::asset toDiamondValue = (eos * (1 * 100)) / 10000; //1 percent to diamond value
            eosio::asset toTreasureOwnerSeller = (eos * (99 * 100)) / 10000; //98 percent to treasure seller
            
            diamondfund_index diamondfund(_self, _self.value);
            auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
            auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
            diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                row.diamondValue += toDiamondValue; //1%
            });

            //Send payment in EOS-tokens to seller 
            action(
                permission_level{ get_self(), "active"_n },
                "eosio.token"_n, "transfer"_n,
                std::make_tuple(get_self(), payToTreasureOwner, 
                                toTreasureOwnerSeller, 
                                std::string("Payment for selling Treasure No." + std::to_string(treasurepkey) + " (1 percent trx fee to The Lost Diamond value)" ))
            ).send(); 

        }
        else if (memo.rfind("BuyBLKBILLTokens:", 0) == 0) { //2020-05-16
            
            asset eosusd = getEosUsdPrice();
            double dblEosUsdPrice = eosusd.amount;
            uint64_t promisedQuantityToBuy = std::strtoull( memo.substr(17).c_str(),NULL,0 ); //The amount of tokens promised to be bought for the amount sent
            uint64_t quantityReached = 0;
            uint64_t usdBuyAmount = eosusd.amount * eos.amount;
            uint64_t usdBuyAmountReached = 0; //Abort if this amount is above the eos-amount sent. The buyer don't get the number of tokens promised for the agreed price
            
            exchngtokens_index exchngtokens(_self, _self.value);
            auto exchngtokensItems = exchngtokens.get_index<"itemprice"_n>();
            auto iter = exchngtokensItems.lower_bound(0);
            while (iter != exchngtokensItems.end()) {
                
                uint64_t totalUsdValueInThisSellOrder = iter->sell.amount * iter->itemprice.amount;
                
                if(usdBuyAmountReached >= usdBuyAmount){
                    break; //We have reached the buy amount for a promised quantity of BLKBILL tokens    
                }
                else if(totalUsdValueInThisSellOrder <= (usdBuyAmount - usdBuyAmountReached)){
                    //All tokens in this sell order can be sold to cover the promised quantity
                    quantityReached += iter->sell.amount;
                    usdBuyAmountReached += totalUsdValueInThisSellOrder;
                    
                    double dblSellAmount = iter->sell.amount;
                    double dblItemPrice = iter->itemprice.amount;
                    double dblVisibleItemPrice = dblItemPrice / 10000;
                    double dblNumberOfTokensSold = dblSellAmount / 10000;
                    double dblTotalSellPriceInEos = (dblSellAmount * dblItemPrice) / dblEosUsdPrice;
                    uint64_t totalSellPriceInEos = dblTotalSellPriceInEos;

                    //std::to_string(dblNumberOfTokensSold).substr(0, std::to_string(dblNumberOfTokensSold).find(".") + 5);
                    //std::to_string(dblNumberOfTokensSold)

                    //Send payment in EOS-tokens to seller 
                    action(
                        permission_level{ get_self(), "active"_n },
                        "eosio.token"_n, "transfer"_n,
                        std::make_tuple(get_self(), iter->account, 
                                        eosio::asset(totalSellPriceInEos, symbol(symbol_code("EOS"), 4)), 
                                        std::string("Payment for selling " + std::to_string(dblNumberOfTokensSold).substr(0, std::to_string(dblNumberOfTokensSold).find(".") + 5) + " BLKBILL tokens. Token price: USD " + std::to_string(dblVisibleItemPrice).substr(0, std::to_string(dblVisibleItemPrice).find(".") + 5)))
                    ).send();

                    iter = exchngtokensItems.erase(iter); //This sell order can be erased

                }
                else if(totalUsdValueInThisSellOrder > (usdBuyAmount - usdBuyAmountReached))
                {
                    //This sell order has more value than needed to cover the promised quantity
                    double usdValueOfTokensFromThisOrderNeeded = usdBuyAmount - usdBuyAmountReached;
                    double dblItemPriceAmount = iter->itemprice.amount;
                    double dblNumberOfTokensFromThisOrderNeeded = usdValueOfTokensFromThisOrderNeeded / dblItemPriceAmount;
                    uint64_t quantityNeededFromSellOrder = dblNumberOfTokensFromThisOrderNeeded; //-5
                    uint64_t restQuantityInSellOrder = iter->sell.amount - quantityNeededFromSellOrder;
                    quantityReached += quantityNeededFromSellOrder;

                    usdBuyAmountReached += (iter->itemprice.amount * quantityNeededFromSellOrder);
                    
                    double dblNumberOfTokensSold = dblNumberOfTokensFromThisOrderNeeded / 10000;
                    double dblItemPrice = iter->itemprice.amount;
                    double dblVisibleItemPrice = dblItemPrice / 10000;
                    double dblTotalSellPriceInEos = (dblNumberOfTokensSold * dblItemPrice) / dblEosUsdPrice;
                    uint64_t totalSellPriceInEos = dblTotalSellPriceInEos * 10000;

                    exchngtokensItems.modify(iter, _self, [&]( auto& row ) {
                        row.sell = eosio::asset(restQuantityInSellOrder, symbol(symbol_code("BLKBILL"), 4));
                    });

                    //Send payment in EOS-tokens to seller 
                    action(
                        permission_level{ get_self(), "active"_n },
                        "eosio.token"_n, "transfer"_n,
                        std::make_tuple(get_self(), iter->account, 
                                        eosio::asset(totalSellPriceInEos, symbol(symbol_code("EOS"), 4)), 
                                        std::string("Payment for selling " + std::to_string(dblNumberOfTokensSold).substr(0, std::to_string(dblNumberOfTokensSold).find(".") + 5) + " BLKBILL tokens.. Token price: USD " + std::to_string(dblVisibleItemPrice).substr(0, std::to_string(dblVisibleItemPrice).find(".") + 5)))
                    ).send();

                    break; 
                } 
            } 

            eosio_assert(quantityReached > 0, "No tokens available.");
            eosio_assert(quantityReached >= (promisedQuantityToBuy * 10000), "Promised token quantity is no longer available.");
            eosio_assert(usdBuyAmountReached <= usdBuyAmount, "Promised token quantity for agreed price is no longer available. Please refresh and try again.");

            //double dblAvgPricePrToken = usdBuyAmountReached / (quantityReached * 10000);
            uint64_t avgPricePrToken = usdBuyAmountReached / quantityReached; //dblAvgPricePrToken; //usdBuyAmountReached / (quantityReached / 10000);
            double dblAvgPricePrToken = avgPricePrToken;
            double dblVisibleAvgPricePrToken = dblAvgPricePrToken / 10000;

            //Transfer BLKBILL quantity to buyer
            action(
                permission_level{ get_self(), "active"_n },
                "cptblackbill"_n, "transfer"_n,
                std::make_tuple(get_self(), 
                                from,  
                                eosio::asset(quantityReached, symbol(symbol_code("BLKBILL"), 4)), 
                                std::string("Buying BLKBILL tokens on Cpt.BlackBill exchange for USD " + std::to_string(dblVisibleAvgPricePrToken).substr(0, std::to_string(dblVisibleAvgPricePrToken).find(".") + 5) + " per token."))
            ).send();

            exchngbuylog_index exchngbuylog(_self, _self.value);
            exchngbuylog.emplace(_self, [&]( auto& row ) {
                row.pkey = exchngbuylog.available_primary_key();
                row.toaccount = from; //The account who sent money receives the tokens
                row.tokens = eosio::asset(quantityReached, symbol(symbol_code("BLKBILL"), 4));
                row.itemprice = eosio::asset(avgPricePrToken, symbol(symbol_code("USD"), 4));
                row.eosprice = getPriceInUSD(eosio::asset(10000, symbol(symbol_code("EOS"), 4))); //Usd price for 1 EOS
                row.timestamp = now();
            });
            
        }
        else if (memo.rfind("RandomChestFunding:", 0) == 0) { //2022-02-10
            eosio_assert(eos >= getPriceForCheckTreasureValueInEOS(), "Transfered amount is below minimum.");

            //The fund will be added to the cptblackbill account and redistributed by a 
            //scheduled task that will fill random checkpoints
            //Valid parameter examples: 
            //RandomChestFunding: (blank)           Will distribute the fund to a random checkpoint
            //RandomChestFunding:{ChestAmount:2}    Will distribute $2 to as many random checkpoints the total amount allow
            //RandomChestFunding:{Spain:50,USA:50}  Will distribute fund 50% to a random checkpoint in Spain and 50% to a random checkpoint in USA 

            rndchestfnd_index rndchestfnd(_self, _self.value);
            rndchestfnd.emplace(_self, [&]( auto& row ) {
                row.pkey = rndchestfnd.available_primary_key();
                row.from = from;
                row.amount = eos;
                row.memo = memo.substr(19).c_str();
                row.executed = false;
                row.timestamp = now();
            });
        }
        else if (memo.rfind("MintCheckpoint:", 0) == 0) { //2022-02-10
            std::string assertMsg = "";
            eosio_assert(eos >= getPriceForCheckTreasureValueInEOS(), "Transfered amount is below minimum.");
            
            //accounts accountstable(eosio::name("cptblackbill"), from.value);
            //const auto& ac = accountstable.get(symbol_code("BLKBILL").raw());
            //assertMsg = "Your BLKBILL balance" + std::to_string(ac.balance.amount);
            //eosio_assert(1 == 2, assertMsg.c_str());
            
            size_t n1 = memo.find(';');
            size_t n2 = memo.find(';', n1 + 1);
            size_t n3 = memo.find(';', n2 + 1);
            size_t n4 = memo.find(';', n3 + 1);
            size_t n5 = memo.find(';', n4 + 1);
            size_t n6 = memo.find(';', n5 + 1);
            size_t n7 = memo.find(';', n6 + 1);
            
            //Memo-format
            //MintCheckpoint:123;title;imageurl;videourl;latitude;longitude;description;

            std::string mintId =               memo.substr(15, n1 - 15);
            std::string title =                memo.substr(n1 + 1, n2 - (n1 + 1));
            std::string imageurl =             memo.substr(n2 + 1, n3 - (n2 + 1));
            std::string videourl =             memo.substr(n3 + 1, n4 - (n3 + 1));
            double latitude = stringtodouble(  memo.substr(n4 + 1, n5 - (n4 + 1))); 
            double longitude = stringtodouble( memo.substr(n5 + 1, n6 - (n5 + 1)));
            std::string description =          memo.substr(n6 + 1, n7 - (n6 + 1));
            
            eosio_assert(title.length() <= 55, "Max length of title is 55 characters.");
            eosio_assert(imageurl.length() <= 100, "Max length of imageUrl is 100 characters.");
            eosio_assert(videourl.length() <= 100, "Max length of videoUrl is 100 characters.");

            bool locationIsValid = true;
            if((latitude < -90 || latitude > 90) || latitude == 0) {
                locationIsValid = false;
            }

            if((longitude < -180 || longitude > 180) || longitude == 0){
                locationIsValid = false;
            }
            
            eosio_assert(locationIsValid, "Location (latitude and/ord longitude) is not valid.");
            int tileZoomLevel = 17;
            int xTile = (int)(floor((longitude + 180.0) / 360.0 * (1 << tileZoomLevel)));
            double latrad = latitude * M_PI/180.0;
	        int yTile = (int)(floor((1.0 - asinh(tan(latrad)) / M_PI) / 2.0 * (1 << tileZoomLevel)));
            
            std::string s1 = std::to_string(xTile) + "." + std::to_string(yTile);
            double tilexy = (double)xTile + ( (double)yTile / pow(10, std::to_string(yTile).length()) ); 

            //Check if map tile is available (not owned by others)
            //std::string testRet = "";
            bool mapTileIsTaken = false;
            eosio::name landOwner;
            treasure_index existingTreasures(_self, _self.value);
            auto idx = existingTreasures.get_index<name("tileidxy"_n)>();  
            auto itrTiles = idx.lower_bound(tilexy); 
            int itrTileCounter = 0;
            
            while (itrTiles != idx.end())
            {
                int existingTileX = (int)(floor((itrTiles->longitude + 180.0) / 360.0 * (1 << tileZoomLevel)));
                double existingLatrad = itrTiles->latitude * M_PI/180.0;
	            int existingTileY = (int)(floor((1.0 - asinh(tan(existingLatrad)) / M_PI) / 2.0 * (1 << tileZoomLevel)));
                if(existingTileX == xTile && existingTileY == yTile){
                    mapTileIsTaken = true;
                    landOwner = name{itrTiles->owner};
                    break;   
                }

                itrTiles++;
                itrTileCounter++;

                if(itrTileCounter > 10) //Lower bound loop will normally find existing tile (if any) at first item. 
                    break;
            }

            if(mapTileIsTaken == true && landOwner == from){
                assertMsg = "You already own this land and have a checkpoint on it. (Map Tile: https://tile.openstreetmap.org/17/" + std::to_string(xTile) + "/" + std::to_string(yTile) + ".png)";
                eosio_assert(1 == 2, assertMsg.c_str());
            }
            else if(mapTileIsTaken == true && landOwner != from){
                assertMsg = "This land (map tile https://tile.openstreetmap.org/17/" + std::to_string(xTile) + "/" + std::to_string(yTile) + ".png) is owned by account " + name{landOwner}.to_string() + ". You are not allowed to create new checkpoints here.";
                eosio_assert(1 == 2, assertMsg.c_str());    
            }

            treasure_index treasures(_self, _self.value);
            
            treasures.emplace(_self, [&]( auto& row ) {
                row.pkey = treasures.available_primary_key();
                row.owner = from;
                row.title = title;
                row.description = description;
                row.imageurl = imageurl;
                row.videourl = videourl;
                row.latitude = latitude;
                row.longitude = longitude;
                row.tileidxy = tilexy;
                row.expirationdate = now() + 94608000; //Treasure expires after three years if not found
                row.status = "active";
                row.timestamp = now();
            });

            //Add payment to diamond fund
            diamondfund_index diamondfund(_self, _self.value);
            auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
            auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
            diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                row.diamondValue += eos; //100%
            });  
        
        }
        else{
            
            /* 2022-02-10 Replace by MintCheckpoint in Transfer
            if (memo.rfind("Activate Treasure No.", 0) == 0){
                eosio_assert(eos >= getPriceForCheckTreasureValueInEOS(), "Transfered amount is below minimum value.");

                uint64_t treasurepkey = std::strtoull( memo.substr(21).c_str(),NULL,0 ); //Find treasure pkey from transfer memo

                treasure_index treasures(_self, _self.value);
                auto iterator = treasures.find(treasurepkey);
                eosio_assert(iterator != treasures.end(), "Treasure not found.");
                //eosio_assert(from == iterator->owner, "Only treasure owner can request treasure activation.");
                eosio_assert(from == iterator->owner || from == iterator->conqueredby, "Only treasure owner or treasure conquerer can request treasure activation.");
                eosio_assert(iterator->status != "active", "Treasure is already activated.");

                treasures.modify(iterator, _self, [&]( auto& row ) {
                    row.status = "requestactivation";
                    if(from == iterator->owner) //Clear conquered by if the treasure owner reactivate the treasure
                        row.conqueredby = ""_n;

                });       
            }
            */

            if(eos >= getPriceForCheckTreasureValueInEOS())
            {
                //Amounts are added to the lost diamond ownership as investment to provision of income 
                //from check- and unlock-transactions. Accounts that activate treasures are also added as lost
                //diamonds owners
                
                //Insert or update invested amount for diamond owner.
                /*diamondownrs_index diamondownrs(_self, _self.value);
                auto account_index = diamondownrs.get_index<name("account")>();
                auto diamondownrsItr = account_index.find(from.value);
                if(diamondownrsItr == account_index.end()){
                    diamondownrs.emplace(_self, [&]( auto& row ) {
                        row.pkey = diamondownrs.available_primary_key();
                        row.account = from;
                        row.investedamount = eos;
                        row.investedpercent = 0; //Null by default. Will be recalculated later
                        row.earnedpayout = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                        row.timestamp = now();
                    });
                }
                else{
                    account_index.modify(diamondownrsItr, _self, [&]( auto& row ) {
                        row.investedamount += eos;
                    });            
                }*/
                
                //Add to diamond fund
                diamondfund_index diamondfund(_self, _self.value);
                auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
                auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);

                if(diamondFundIterator->foundTimestamp == 0){
                    //present diamond has not been found. Add value to existing diamond
                    diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                        row.diamondValue += eos; //100%
                    });  
                }
                else{
                    //present diamond has been found. Create new diamond and add transferred value to new diamond
                    //diamondfund_index diamondfund(_code, _code.value);
                    diamondfund.emplace(_self, [&]( auto& row ) { 
                        row.pkey = diamondfund.available_primary_key();
                        row.toDiamondOwners = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                        row.toTokenHolders = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                        row.diamondValue = eos; 
                        row.foundTimestamp = 0;
                    });
                }

                //cptblackbill::issue(from, eosio::asset(10, symbol(symbol_code("BLKBILL"), 4)), std::string("Mined BLKBILLs for investing in the lost diamond.") );
            
            }
            else{
                //All other smaller amounts will initiate BLKBILL token issue
                //cptblackbill::issue(from, eosio::asset(1, symbol(symbol_code("BLKBILL"), 4)), std::string("Mined BLKBILLS for using Captain Black Bill.") );
            
                //eosio_assert(1 == 0 , "Invalid transfer to cpt.blackbill smart contract. Minimum amount is $1.");
            }
        }
    }
    //=====================================================================

    bool replace(std::string& str, const std::string& from, const std::string& to) {
        size_t start_pos = str.find(from);
        if(start_pos == std::string::npos)
            return false;
        str.replace(start_pos, from.length(), to);
        return true;
    }

    //2022-02-10
    double stringtodouble(std::string str)
    {
        double dTmp = 0.0;
        bool isNegative = false;
        int iLen = str.length();
        int iPos = str.find(".");
        std::string strIntege = str.substr(0,iPos);
        std::string strDecimal = str.substr(iPos + 1,iLen - iPos - 1 );
        
        if (strIntege[0] == '-')
            isNegative = true;

        for (int i = 0; i < iPos;i++)
        {
            if (strIntege[i] >= '0' && strIntege[i] <= '9')
            {
                dTmp = dTmp * 10 + strIntege[i] - '0';
            }
        }
        
        for (int j = 0; j < strDecimal.length(); j++)
        {
            if (strDecimal[j] >= '0' && strDecimal[j] <= '9')
            {
                dTmp += (strDecimal[j] - '0') * pow(10.0,(0 - j - 1));
            }
        }

        if(isNegative)
            dTmp = dTmp * -1;

        return dTmp;
    } 

    /* 2022-07-04 Add treasure is replaced with MintCheckpoint in transfer function 
    [[eosio::action]]
    void addtreasure(eosio::name owner, std::string title, std::string imageurl, 
                     double latitude, double longitude) 
    {
        require_auth(owner);
        
        eosio_assert(title.length() <= 55, "Max length of title is 55 characters.");
        eosio_assert(imageurl.length() <= 100, "Max length of imageUrl is 100 characters.");

        bool locationIsValid = true;
        if((latitude < -90 || latitude > 90) || latitude == 0) {
            locationIsValid = false;
        }

        if((longitude < -180 || longitude > 180) || longitude == 0){
            locationIsValid = false;
        }
        
        eosio_assert(locationIsValid, "Location (latitude and/ord longitude) is not valid.");
        
        treasure_index treasures(_code, _code.value);
        
        treasures.emplace(owner, [&]( auto& row ) {
            row.pkey = treasures.available_primary_key();
            row.owner = owner;
            row.title = title;
            row.imageurl = imageurl;
            row.latitude = latitude;
            row.longitude = longitude;
            row.expirationdate = now() + 94608000; //Treasure expires after three years if not found
            row.status = "created";
            row.timestamp = now();
        });
    } */

    //2021-10-03 For selling a treasure. The treasure owner can add a asking price for the treasure
    [[eosio::action]]
    void addsellprice(eosio::name treasureowner, uint32_t treasurepkey, asset askingpriceUsd, std::string memo) 
    {
        require_auth(treasureowner);
        
        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(treasurepkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        eosio_assert(treasureowner == iterator->owner, "You are not the owner of this treasure.");
        eosio_assert(askingpriceUsd.symbol == symbol(symbol_code("USD"), 4), "Asking price must be in USD.");
        eosio_assert(askingpriceUsd.amount >= 10000, "Asking price can not be less than one dollar.");
        
        treasuresale_index treasuresales(_code, _code.value);
        auto idxTreasureSales = treasuresales.get_index<name("treasurepkey")>();
        auto treasuresaleIterator = idxTreasureSales.find(treasurepkey); 
        if(treasuresaleIterator == idxTreasureSales.end()){
            treasuresales.emplace(treasureowner, [&]( auto& row ) {
                row.pkey = treasuresales.available_primary_key();
                row.account = treasureowner;
                row.treasurepkey = treasurepkey;
                row.askingpriceUsd = askingpriceUsd;
                row.memo = memo;
                row.expirationdate = now() + 31536000; //Asking price expires after one year
                row.timestamp = now();
            });
        }
        else{
            idxTreasureSales.modify(treasuresaleIterator, _self, [&]( auto& row ) {
                row.askingpriceUsd = askingpriceUsd;
                row.memo = memo;
                row.expirationdate = now() + 31536000; //Update asking price expires after one year
            });            
        }
    }    
    
    [[eosio::action]]
    void delsellprice(eosio::name treasureowner, uint32_t treasurepkey) 
    {
        require_auth(treasureowner);

        treasure_index treasures(_code, _code.value);
        auto treasureIterator = treasures.find(treasurepkey);
        eosio_assert(treasureIterator != treasures.end(), "Treasure not found.");
        eosio_assert(treasureowner == treasureIterator->owner, "You are not the owner of this treasure.");
        
        treasuresale_index treasuresales(_code, _code.value);
        auto idxTreasureSales = treasuresales.get_index<name("treasurepkey")>();
        //auto treasuresaleIterator = idxTreasureSales.find(treasurepkey); // treasuresales.find(treasurepkey);
        auto treasuresaleIterator = idxTreasureSales.find(treasurepkey); // lower_bound(treasurepkey) or treasuresales.find(treasurepkey);
        
        eosio_assert(treasuresaleIterator != idxTreasureSales.end(), "No active sell price found. Tresure is not for sale.");
        while(treasuresaleIterator != idxTreasureSales.end()) {
            if(treasuresaleIterator->account == treasureowner && treasuresaleIterator->treasurepkey == treasurepkey)
                treasuresaleIterator = idxTreasureSales.erase(treasuresaleIterator);
            else
                treasuresaleIterator++;    
        }   
    }    

    //2020-09-29 For airdropping blkbill tokens and awarding users 
    [[eosio::action]]        
    void airdrop(name toaccount, asset blkbills, std::string memo) 
    {
        require_auth("cptblackbill"_n); //Only allowed by cptblackbill
        cptblackbill::issue(toaccount, blkbills, memo);
    }

    [[eosio::action]]
    void addteammbr(eosio::name teamMember, std::string youTubeName) 
    {
        require_auth("cptblackbill"_n); //Only allowed by cptblackbill 
        
        teambearland_index teambearland(_code, _code.value);
        teambearland.emplace(_self, [&]( auto& row ) {
            row.pkey = teambearland.available_primary_key();
            row.teamMember = teamMember;
            row.youTubeName = youTubeName;
        });
    }

    [[eosio::action]]
    void delteammbr() {
        require_auth("cptblackbill"_n);
        
        uint64_t counter = 0;
        teambearland_index teambearland(_self, _self.value);
        auto itr = teambearland.begin();
        while(itr != teambearland.end()) {
            itr = teambearland.erase(itr);

            counter++;
            if(counter >= 500) //Prevent exceed cpu usage
                break;
        } 
    }

    [[eosio::action]]
    void addtradmin(uint64_t pkey, eosio::name owner, std::string title, std::string description, std::string treasuremapurl,
                     std::string imageurl, std::string videourl, double latitude, double longitude, uint64_t rankingpoint,
                     std::string status, uint32_t expirationdate, uint32_t timestamp) 
    {
        require_auth("cptblackbill"_n); //Only allowed by cptblackbill 
        
        eosio_assert(title.length() <= 55, "Max length of title is 55 characters.");
        eosio_assert(imageurl.length() <= 100, "Max length of imageUrl is 100 characters.");

        bool locationIsValid = true;
        if((latitude < -90 || latitude > 90) || latitude == 0) {
            locationIsValid = false;
        }

        if((longitude < -180 || longitude > 180) || longitude == 0){
            locationIsValid = false;
        }
        
        eosio_assert(locationIsValid, "Location (latitude and/ord longitude) is not valid.");
        
        treasure_index treasures(_code, _code.value);
        
        treasures.emplace(_self, [&]( auto& row ) {
            row.pkey = pkey;
            row.owner = owner;
            row.title = title;
            row.description = description;
            row.imageurl = imageurl;
            row.treasuremapurl = treasuremapurl;
            row.videourl = videourl;
            row.latitude = latitude;
            row.longitude = longitude;
            row.rankingpoint = rankingpoint;
            row.expirationdate = expirationdate; 
            row.status = status;
            row.timestamp = timestamp;
        });
    }

    [[eosio::action]]
    void modtreasure(name user, uint64_t pkey, std::string title, std::string description, std::string imageurl, 
                     std::string videourl) 
    {
        require_auth( user );
        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(pkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        eosio_assert(user == iterator->owner || user == iterator->conqueredby || user == "cptblackbill"_n, "You don't have access to modify this treasure.");

        eosio_assert(title.length() <= 55, "Max length of title is 55 characters.");
        eosio_assert(description.length() <= 650, "Max length of description is 650 characters.");
        eosio_assert(imageurl.length() <= 100, "Max length of image url is 100 characters.");
        eosio_assert(videourl.length() <= 100, "Max length of video url is 100 characters.");
        
        treasures.modify(iterator, _self, [&]( auto& row ) {
            row.title = title;
            row.description = description;
            row.videourl = videourl;

            //row.imageurl = imageurl;
            if(user == iterator->conqueredby)
                row.conqueredimg = imageurl;
            else
                row.imageurl = imageurl;
        });
    }

    [[eosio::action]]
    void moddmndval(asset valueInEos) 
    {
        require_auth("cptblackbill"_n); //Only allowed by cptblackbill contract

        //Modify diamond value to correct amount in EOS
        //Used by cptblackbill account if diamond value exceeds actual amount on account or if something is wrong.
        diamondfund_index diamondfund(_self, _self.value);
        auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
        auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
        diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
            row.diamondValue = valueInEos; 
        });  
    }

    [[eosio::action]]
    void modtreasimg(name user, uint64_t pkey, std::string imageurl) 
    {
        require_auth( user );
        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(pkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        //eosio_assert(user == iterator->owner || user == "cptblackbill"_n, "You don't have access to modify this treasure.");
        eosio_assert(user == iterator->owner || user == iterator->conqueredby || user == "cptblackbill"_n, "You don't have access to modify this treasure.");
        eosio_assert(imageurl.length() <= 100, "Max length of image url is 100 characters.");
        
        treasures.modify(iterator, user, [&]( auto& row ) {
            if(user == iterator->conqueredby)
                row.conqueredimg = imageurl;
            else
                row.imageurl = imageurl;
        });
    }

    [[eosio::action]]
    void modgps(name user, uint64_t pkey, double latitude, double longitude) 
    {
        require_auth( user );
        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(pkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        eosio_assert(user == iterator->owner || user == iterator->conqueredby || user == "cptblackbill"_n, "You don't have access to modify this treasure.");
        
        bool locationIsValid = true;
        if((latitude < -90 || latitude > 90) || latitude == 0) {
            locationIsValid = false;
        }

        if((longitude < -180 || longitude > 180) || longitude == 0){
            locationIsValid = false;
        }
        
        eosio_assert(locationIsValid, "Location (latitude and/ord longitude) is not valid.");

        //Get map tilexy id
        int tileZoomLevel = 17;
        int xTile = (int)(floor((longitude + 180.0) / 360.0 * (1 << tileZoomLevel)));
        double latrad = latitude * M_PI/180.0;
        int yTile = (int)(floor((1.0 - asinh(tan(latrad)) / M_PI) / 2.0 * (1 << tileZoomLevel)));
        std::string s1 = std::to_string(xTile) + "." + std::to_string(yTile);
        double tilexy = (double)xTile + ( (double)yTile / pow(10, std::to_string(yTile).length()) ); // (std::to_string(yTile).length() * 10);

        treasures.modify(iterator, user, [&]( auto& row ) {
            row.latitude = latitude;
            row.longitude = longitude;
            row.tileidxy = tilexy;
        });
    }

    [[eosio::action]]
    void modtreasjson(name user, uint64_t pkey, std::string jsondata) 
    {
        require_auth( user );
        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(pkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        eosio_assert(user == "cptblackbill"_n, "You don't have access to modify this treasure.");

        treasures.modify(iterator, user, [&]( auto& row ) {
            row.jsondata = jsondata;
        });
    }

    [[eosio::action]]
    void modsecretcode(name user, uint64_t pkey, std::string encryptedSecretCode) 
    {
        require_auth( user );
        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(pkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        eosio_assert(user == "cptblackbill"_n, "You don't have access to modify secret code on this treasure.");

        treasures.modify(iterator, user, [&]( auto& row ) {
            row.secretcode = encryptedSecretCode;
        });
    }

    //[[eosio::action]]
    //void signin2fa(name user) 
    //{
    //    require_auth(user);
    //}

    [[eosio::action]]
    void activatchest(uint64_t pkey, std::string encryptedSecretCode) 
    {
        require_auth("cptblackbill"_n); //Only allowed by cptblackbill contract

        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(pkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        
        //Get number of unique accounts that has solved treasures created by treasure owner = rating points.
        //2022-12-29 Removed. Ranking points are calculated in front end form.
        //uint64_t numberOfUniqueUserUnlocks = 0;
        //std::set<uint64_t> uniqueUsersSet;

        //results_index results(_code, _code.value);
        //auto resultItems = results.get_index<"creator"_n>();
        //auto iter = resultItems.find(iterator->owner.value);
        //while (iter != resultItems.end()) {
        //    if(iter->creator != iterator->owner) //Stop when list is outside the values of the treasures creator
        //        break;
        //    
        //    uniqueUsersSet.insert(iter->user.value); //insert only add unique values to a set
        //    iter++;
        //} 
        //numberOfUniqueUserUnlocks = uniqueUsersSet.size();
        
        treasures.modify(iterator, _self, [&]( auto& row ) {
            row.status = "active";
            row.secretcode = encryptedSecretCode;
            //row.rankingpoint = numberOfUniqueUserUnlocks; 2022-12-29 Removed
            row.expirationdate = now() + 94608000; //Treasure ownership renewed for three years
        });
    }

    //2022-12-29 updranking. Ranking points are calculated by several criterias in a front end view.
    [[eosio::action]]
    void updranking(uint64_t pkey, uint64_t rankingPoints) 
    {
        require_auth("cptblackbill"_n); //Only allowed by cptblackbill contract

        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(pkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        
        treasures.modify(iterator, _self, [&]( auto& row ) {
            row.rankingpoint = rankingPoints;
        });
    }
    /*
    void updranking(uint64_t pkey) 
    {
        require_auth("cptblackbill"_n); //Only allowed by cptblackbill contract

        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(pkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        
        //Get number of unique accounts that has solved treasures created by treasure owner = rating points.
        uint64_t numberOfUniqueUserUnlocks = 0;
        uint64_t counter = 0;
        std::set<uint64_t> uniqueUsersSet;

        results_index results(_code, _code.value);
        auto resultItems = results.get_index<"creator"_n>();
        auto iter = resultItems.find(iterator->owner.value); //makes the list start at lower bound for the treasures creator
        while (iter != resultItems.end()) {
            if(iter->creator != iterator->owner) //Stop when list is outside the values of the treasures creator
                break;
            
            uniqueUsersSet.insert(iter->user.value); //insert only add unique values to a set
            iter++;
        } 
        numberOfUniqueUserUnlocks = uniqueUsersSet.size();
        
        treasures.modify(iterator, _self, [&]( auto& row ) {
            row.rankingpoint = numberOfUniqueUserUnlocks;
        });
    } */

    [[eosio::action]]
    void unlockchest(uint64_t treasurepkey, asset payouteos, name byuser, bool lostdiamondisfound, 
                     uint64_t sponsoritempkey, name teammember) { //bool isNoPaymentRobbery
        require_auth("cptblackbill"_n); //Only allowed by cptblackbill contract

        //Get total amount in Lost Diamond if diamond is found in this treasure
        //eosio::asset totalamountinlostdiamond = eosio::asset(0, symbol(symbol_code("EOS"), 4));
        if(lostdiamondisfound){
            //tcrfund_index tcrfund(_self, _self.value);
            //auto itr = tcrfund.upper_bound(0);
            //for (auto itr = tcrfund.begin(); itr != tcrfund.end(); itr++) {
            //    totalamountinlostdiamond = totalamountinlostdiamond + (*itr).investedamount;
            //}

            //2020-02-29
            diamondfund_index diamondfund(_self, _self.value);
            auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
            auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
            eosio_assert(diamondFundIterator->foundTimestamp == 0, "Diamond is already found.");
            payouteos = payouteos + diamondFundIterator->diamondValue; //Add lost diamond value to the treasure value
        }

        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(treasurepkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        name treasureowner = iterator->owner; 
        name treasureConquerer = iterator->conqueredby;

        treasures.modify(iterator, _self, [&]( auto& row ) {
            row.status = "active";
            
            if(byuser != treasureowner)
                row.conqueredby = byuser; //The treasure has been conquered by the robber. The robber has now access to activate the treasure with a new code.
            else
                row.conqueredby = ""_n; //Set as blank if owner has conquered back the treasure.

            
            //Reward finder for using CptBlackBill. 10000 = 1 BLKBILL
            cptblackbill::issue(byuser, eosio::asset(10, symbol(symbol_code("BLKBILL"), 4)), std::string("Reward for unlocking treasure.") );
            //send_summary(byuser, "1 BLKBILL token as reward for unlocking a treasure.");

            //Reward creator for creating content 
            cptblackbill::issue(treasureowner, eosio::asset(1, symbol(symbol_code("BLKBILL"), 4)), std::string("Reward for someone unlocking your treasure.") );
            //send_summary(treasureowner, "2 BLKBILL token as reward for someone unlocking your treasure.");
            
            //Update 2018-12-28 Add user who unlocked tresure to the result table for easy access on scoreboard in dapp
            results_index results(_code, _code.value);
            results.emplace(_self, [&]( auto& row ) { 
                row.pkey = results.available_primary_key();
                row.user = byuser; //The eos account that found and unlocked the treasure
                row.creator = treasureowner; //The eos account that created or owns the treasure
                row.conqueredby = treasureConquerer; //The eos account that has conquered the treasure and share 50/50 with owner
                row.treasurepkey = treasurepkey;
                row.lostdiamondfound = lostdiamondisfound;
                row.payouteos = payouteos;

                //if(isNoPaymentRobbery == true)
                //    row.eosusdprice = eosio::asset(0, symbol(symbol_code("USD"), 4)); //2020-04-10 Zero to mark that this is a no payment robbery
                //else
                row.eosusdprice = getEosUsdPrice(); //2019-01-08
                
                row.minedblkbills = eosio::asset(10, symbol(symbol_code("BLKBILL"), 4));
                row.timestamp = now();
            });

            //2020-11-29 Conquer is free if correct code. Send payed unlock fee back to user
            //action(
            //    permission_level{ get_self(), "active"_n },
            //    "eosio.token"_n, "transfer"_n,
            //    std::make_tuple(get_self(), byuser, payQuantity, std::string("Payment returned for successful conquering Treasure No." + std::to_string(treasurepkey) + " on CptBlackBill."))
            //).send();

            if(payouteos.amount > 0) 
            {
                //Treasure has been unlocked by <byuser>. 
                //Split payout amount in two - since both creator and finder get an equal share of the treasure
                payouteos = payouteos / 2;

                //Transfer treasure chest value to the user who unlocked the treasure
                if(byuser == "bearland.gm"_n && is_account(teammember)){ //Payout to team-member of bearland.gm 2022-08-05
                    action(
                        permission_level{ get_self(), "active"_n },
                        "eosio.token"_n, "transfer"_n,
                        std::make_tuple(get_self(), teammember, payouteos, std::string("The Lost Diamond Adventure Race. Congrats for solving checkpoint No." + std::to_string(treasurepkey) + " as BearLand team-member!"))
                    ).send();
                }
                else{
                    action(
                        permission_level{ get_self(), "active"_n },
                        "eosio.token"_n, "transfer"_n,
                        std::make_tuple(get_self(), byuser, payouteos, std::string("Congrats for solving checkpoint No." + std::to_string(treasurepkey) + " on The Lost Diamond!"))
                    ).send();
                }
                

                //Transfer the same amount to the user who created the treasure
                //Share the amount with the conquerer if the treasure has a conquerer
                if(byuser == treasureowner && is_account(treasureConquerer)){
                    //The treasure owner conquer back ownership to the treasure. Then send the other half to conquerer
                    action(
                        permission_level{ get_self(), "active"_n },
                        "eosio.token"_n, "transfer"_n,
                        std::make_tuple(get_self(), treasureConquerer, payouteos, std::string("Congrats! Checkpoint No." + std::to_string(treasurepkey) + " has been solved by the owner. This is your equal share of the treasure chest."))
                    ).send();
                }
                else if(is_account(treasureConquerer)){
                    asset payouteosToOwner = eosio::asset(payouteos.amount/2, symbol(symbol_code("EOS"), 4));
                    asset payouteosToConquerer = eosio::asset(payouteos.amount/2, symbol(symbol_code("EOS"), 4));

                    if(payouteosToOwner.amount > 0)
                    { 
                        action(
                            permission_level{ get_self(), "active"_n },
                            "eosio.token"_n, "transfer"_n,
                            std::make_tuple(get_self(), treasureowner, payouteosToOwner, std::string("Congrats! Your Treasure No." + std::to_string(treasurepkey) + " has been solved. You share 50/50 with the current conquerer."))
                        ).send();
                    }

                    if(payouteosToConquerer.amount > 0)
                    { 
                        action(
                            permission_level{ get_self(), "active"_n },
                            "eosio.token"_n, "transfer"_n,
                            std::make_tuple(get_self(), treasureConquerer, payouteosToConquerer, std::string("Congrats! Your conquered treasure No." + std::to_string(treasurepkey) + " has been solved. You share 50/50 with the owner."))
                        ).send();
                    }
                }
                else{
                    action(
                        permission_level{ get_self(), "active"_n },
                        "eosio.token"_n, "transfer"_n,
                        std::make_tuple(get_self(), treasureowner, payouteos, std::string("Congrats! Your Treasure No." + std::to_string(treasurepkey) + " has been solved. This is your equal share of the treasure chest."))
                    ).send();
                }                

                if(lostdiamondisfound){
                    
                    //2020-02-29 Mark diamond as found (This will replace the code below.)
                    //This will mark that preparation for payout starts and a new diamond fund is created when 
                    //current diamond owners payout is calculated. 
                    diamondfund_index diamondfund(_code, _code.value);
                    auto itr = diamondfund.rbegin(); //Find the last added diamond fund item
                    auto iterator = diamondfund.find(itr->pkey);
                    diamondfund.modify(iterator, _self, [&]( auto& row ) {
                        row.foundTimestamp = now();
                        row.foundInTreasurePkey = treasurepkey;
                        row.foundByaccount = byuser;
                    });                     
                    
                    //Send earned income to the current lost diamond owners and delete rows
                    /*tcrfund_index tcrfund(_self, _self.value);
                    for (auto itr = tcrfund.begin(); itr != tcrfund.end(); itr++) {
                        
                        //Send payout to all diamond owners except for cptblackbill
                        //CptBlackBill's provision will be distributed to a random treasure
                        if(itr->earnedpayout.amount > 0 && itr->account != "cptblackbill"_n) 
                        {
                            action(
                                permission_level{ get_self(), "active"_n },
                                "eosio.token"_n, "transfer"_n,
                                std::make_tuple(get_self(), itr->account, itr->earnedpayout, std::string("Income for The Lost Diamond owners."))
                            ).send();
                        }

                        tcrfund.modify(itr, _self, [&]( auto& row ) {
                            row.earnedpayout = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                            row.investedamount = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                            row.investorpercent = 0; 
                        }); 
                    }*/
                } 
            }
        });

        if(sponsoritempkey > 0){ //Sponsor item pKey must always be larger than 0. 
            sponsoritems_index sponsoritems(_self, _self.value);
            auto iterator = sponsoritems.find(sponsoritempkey);
            asset oneThirdOfAdFeePrice = iterator->adFeePrice / 3;
            sponsoritems.modify(iterator, _self, [&]( auto& row ) {
                row.status = "robbed";
                row.wonby = byuser;
                row.treasurepkey = treasurepkey;
                row.wontimestamp = now();
            }); 

            diamondfund_index diamondfund(_self, _self.value);
            auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
            auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
            diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                row.toTokenHolders += oneThirdOfAdFeePrice;
            });  

            //Share earned advertising amount with the conquerer if the treasure has a conquerer
            if(is_account(treasureConquerer)){
                action(
                    permission_level{ get_self(), "active"_n },
                    "eosio.token"_n, "transfer"_n,
                    std::make_tuple(get_self(), treasureowner, oneThirdOfAdFeePrice, std::string("Earned advertising fee on Treasure No." + std::to_string(treasurepkey)))
                ).send();

                //Transfer earned fee to treasure owner
                action(
                    permission_level{ get_self(), "active"_n },
                    "eosio.token"_n, "transfer"_n,
                    std::make_tuple(get_self(), treasureConquerer, oneThirdOfAdFeePrice, std::string("Earned advertising fee on Treasure No." + std::to_string(treasurepkey)))
                ).send();
            }
            else{
                action(
                    permission_level{ get_self(), "active"_n },
                    "eosio.token"_n, "transfer"_n,
                    std::make_tuple(get_self(), treasureowner, oneThirdOfAdFeePrice * 2, std::string("Earned advertising fee on Treasure No." + std::to_string(treasurepkey)))
                ).send();
            }
        }
    }

    //2020-06-29: For adding race result (members cup and public events) to the result table (to show up on the leaderboard).
    [[eosio::action]]
    void addresult(name raceparticipant, name raceowner, uint32_t totalpoints, uint32_t endracetimestamp) {
        require_auth("cptblackbill"_n);
    
        eosio_assert(totalpoints > 0, "Total points must be larger than 0");

        //Reward race participant. Points = BLKBILLs / 10000. 
        cptblackbill::issue(raceparticipant, eosio::asset(totalpoints, symbol(symbol_code("BLKBILL"), 4)), std::string("Reward for racing.") );
        
        //Reward race owner with 1 BLKBILL tokens pr participant. (if using same amount as participant, the race owner will set points to max on all checkpoints). 
        cptblackbill::issue(raceowner, eosio::asset(10000, symbol(symbol_code("BLKBILL"), 4)), std::string("Reward for hosting race event.") );
            
        //Add participant to result table. Points == mined black bills
        results_index results(_code, _code.value);
        results.emplace(_self, [&]( auto& row ) { 
            row.pkey = results.available_primary_key();
            row.user = raceparticipant; //The eos account that found and unlocked the treasure
            row.creator = raceowner; //The eos account that created or owns the treasure
            row.treasurepkey = 21; //Race event treasure (pkey=21 does not exists as treasure)
            row.lostdiamondfound = 0;
            row.payouteos = eosio::asset(0, symbol(symbol_code("EOS"), 4));;
            row.eosusdprice = eosio::asset(0, symbol(symbol_code("USD"), 4)); //2020-04-10 Zero to mark that this is a no payment robbery (in this case racing event)
            row.minedblkbills = eosio::asset(totalpoints, symbol(symbol_code("BLKBILL"), 4));
            row.timestamp = endracetimestamp;
        });
    }

    [[eosio::action]]
    void awardpayout(uint64_t yyyymm, name fpAccount, uint32_t fpPoints, name spAccount, uint32_t spPoints, name tpAccount, uint32_t tpPoints) {
        require_auth("cptblackbill"_n);

        //Check if this month already exists. End with error msg if exists
        resultsmnth_index resultsmnth(_code, _code.value);
        auto resultmnthItr = resultsmnth.find(yyyymm);
        eosio_assert(resultmnthItr == resultsmnth.end(), "Monthly ranking award already exists.");

        //Get current diamond value
        diamondfund_index diamondfund(_self, _self.value);
        auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
        auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
        asset diamondValue = diamondFundIterator->diamondValue;
        uint64_t diamondPkey = diamondFundIterator->pkey; 
        eosio_assert(diamondFundIterator->foundTimestamp == 0, "Diamond is already found. No prize available.");
    
        //Get award amount for first, second and third place
        double firstPlaceAward = (diamondValue.amount * 2.5) / 100; //2.5 percent (is actually 5 percent of the diamond value)
        double secondPlaceAward = (diamondValue.amount * 1.5) / 100; //1.5 percent (3 percent)
        double thirdPlaceAward = (diamondValue.amount * 1) / 100; //1 percent (2 percent)
        double totalAwardInEos = firstPlaceAward + secondPlaceAward + thirdPlaceAward;

        uint64_t intFirstPlaceaward = firstPlaceAward;
        uint64_t intSecondPlaceAward = secondPlaceAward;
        uint64_t intThirdPlaceAward = thirdPlaceAward;
        uint64_t intRemainingDiamondValue = diamondValue.amount; 
        
        asset eosFirstPlaceaward = eosio::asset(intFirstPlaceaward, symbol(symbol_code("EOS"), 4));
        asset eosSecondPlaceAward = eosio::asset(intSecondPlaceAward, symbol(symbol_code("EOS"), 4));
        asset eosThirdPlaceAward = eosio::asset(intThirdPlaceAward, symbol(symbol_code("EOS"), 4));
        
        resultsmnth.emplace(_self, [&]( auto& row ) { 
            row.pkey = yyyymm; 
            row.fpAccount = fpAccount;
            row.fpPoints = fpPoints;
            row.fpEos = eosFirstPlaceaward;
            row.spAccount = spAccount;
            row.spPoints = spPoints;
            row.spEos = eosSecondPlaceAward;
            row.tpAccount = tpAccount;
            row.tpPoints = tpPoints;
            row.tpEos = eosThirdPlaceAward;
            row.eosusdprice = getEosUsdPrice();
            row.timestamp = now();
        });

        //Payout to fp, sp and tp
        if(fpPoints > 0 && intFirstPlaceaward > 0)
        { 
            action(
                permission_level{ get_self(), "active"_n },
                "eosio.token"_n, "transfer"_n,
                std::make_tuple(get_self(), fpAccount, eosFirstPlaceaward, std::string("Congrats! You won the last month competition with " + std::to_string(fpPoints) + " points."))
            ).send();
            intRemainingDiamondValue = intRemainingDiamondValue - intFirstPlaceaward;
        }

        if(spPoints > 0 && intSecondPlaceAward > 0)
        { 
            action(
                permission_level{ get_self(), "active"_n },
                "eosio.token"_n, "transfer"_n,
                std::make_tuple(get_self(), spAccount, eosSecondPlaceAward, std::string("Congrats! You won second place in the last month competition with " + std::to_string(spPoints) + " points."))
            ).send();
            intRemainingDiamondValue = intRemainingDiamondValue - intSecondPlaceAward;
        }

        if(tpPoints > 0 && intThirdPlaceAward > 0)
        { 
            action(
                permission_level{ get_self(), "active"_n },
                "eosio.token"_n, "transfer"_n,
                std::make_tuple(get_self(), tpAccount, eosThirdPlaceAward, std::string("Congrats! You won third place in the last month competition with " + std::to_string(tpPoints) + " points."))
            ).send();
            intRemainingDiamondValue = intRemainingDiamondValue - intThirdPlaceAward;
        }

        //Update new amount for diamond value
        asset eosRemainingDiamondValue = eosio::asset(intRemainingDiamondValue, symbol(symbol_code("EOS"), 4));
        diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
            row.diamondValue = eosRemainingDiamondValue;
        });
    }

    [[eosio::action]]
    void btulla(name byuser, uint64_t fromPkey, asset testeos, uint64_t toPkey) {
        require_auth("cptblackbill"_n);

        //Add TileIdxy on treasures
        /*std::string testRet = "";
        uint64_t counter = 0;
        treasure_index treasures(_code, _code.value);
        auto treasuresItr = treasures.begin();
        while(treasuresItr != treasures.end()) {
            
            int tileZoomLevel = 17;
            int xTile = (int)(floor((treasuresItr->longitude + 180.0) / 360.0 * (1 << tileZoomLevel)));
            double latrad = treasuresItr->latitude * M_PI/180.0;
	        int yTile = (int)(floor((1.0 - asinh(tan(latrad)) / M_PI) / 2.0 * (1 << tileZoomLevel)));

            std::string s1 = std::to_string(xTile) + "." + std::to_string(yTile);
            double tilexy = (double)xTile + ( (double)yTile / pow(10, std::to_string(yTile).length()) ); // (std::to_string(yTile).length() * 10);

            treasures.modify(treasuresItr, byuser, [&]( auto& row ) {
                row.tileidxy = tilexy;
            });  

            treasuresItr++;

            counter++;
            if(counter >= fromPkey)
                break;   
        } */

        //eosio_assert(1 == 2, testRet.c_str());
        
        

        //require_auth("cptblackbill"_n); //"Updating expiration date is only allowed by CptBlackBill. This is to make sure (verified gps location by CptBlackBill) that the owner has actually been on location and entered secret code
        //treasure_index treasures(_code, _code.value);
        //auto iterator = treasures.find(pkey);
        //eosio_assert(iterator != treasures.end(), "Treasure not found");
        
        //treasures.modify(iterator, user, [&]( auto& row ) {
        //    row.expirationdate = now() + 94608000; //Treasure ownership renewed for three years
        //});

        
        //Remove checkpoints rows
        /*checkpoint_index checkpointsd(_self, _self.value);
        auto checkpointsItr = checkpointsd.begin();
        while(checkpointsItr != checkpointsd.end()) {
            checkpointsItr = checkpointsd.erase(checkpointsItr);
        } */

        //Remove treasures rows
        /*treasure_index treasures(_self, _self.value);
        auto treasuresItr = treasures.begin();
        while(treasuresItr != treasures.end()) {
            treasuresItr = treasures.erase(treasuresItr);
        } */
        
        //Copy all treasures to checkpoints
        /*treasure_index treasures(_self, _self.value);
        checkpoint_index checkpoints(_self, _self.value);
        for (auto itr = treasures.begin(); itr != treasures.end(); itr++) {
            checkpoints.emplace(_self, [&]( auto& row ) {
                row.pkey = itr->pkey;
                row.owner = itr->owner;
                row.title = itr->title;
                row.description = itr->description;
                row.imageurl = itr->imageurl;
                row.treasuremapurl = itr->treasuremapurl;
                row.videourl = itr->videourl;
                row.latitude = itr->latitude;
                row.longitude = itr->longitude;
                row.tileidxy = itr->tileidxy;
                row.rankingpoint = itr->rankingpoint;
                row.timestamp = itr->timestamp;
                row.expirationdate = itr->expirationdate;
                row.secretcode = itr->secretcode;
                row.status = itr->status;
                row.banditalarms = 0;
                row.noOfCaptures = 0;
                row.ctypeid = 0;
                row.conqueredby = itr->conqueredby;
                row.conqueredimg = itr->conqueredimg;
                row.jsondata = itr->jsondata;
            });
        } */

        //Copy all checkpoints to treasures 
        /*checkpoint_index checkpoints(_self, _self.value);
        treasure_index treasures(_self, _self.value);
        for (auto itr = checkpoints.begin(); itr != checkpoints.end(); itr++) {
            treasures.emplace(_self, [&]( auto& row ) {
                row.pkey = itr->pkey;
                row.owner = itr->owner;
                row.title = itr->title;
                row.description = itr->description;
                row.imageurl = itr->imageurl;
                row.treasuremapurl = itr->treasuremapurl;
                row.videourl = itr->videourl;
                row.latitude = itr->latitude;
                row.longitude = itr->longitude;
                row.tileidxy = itr->tileidxy;
                row.rankingpoint = itr->rankingpoint;
                row.timestamp = itr->timestamp;
                row.expirationdate = itr->expirationdate;
                row.secretcode = itr->secretcode;
                row.status = itr->status;
                row.banditalarms = itr->banditalarms;
                row.noOfCaptures = itr->noOfCaptures;
                row.ctypeid = 0;
                row.conqueredby = itr->conqueredby;
                row.conqueredimg = itr->conqueredimg;
                row.jsondata = itr->jsondata;
            });
        } */

        //Remove exchngbuylog rows
        //exchngbuylog_index exchngbuylog(_self, _self.value);
        //auto buylogItr = exchngbuylog.begin();
        //while(buylogItr != exchngbuylog.end()) {
        //    buylogItr = exchngbuylog.erase(buylogItr);
        //} 

        //Remove exchngtokens rows
        //exchngtokens_index exchngtokens(_self, _self.value);
        //auto exchngtokensItr = exchngtokens.begin();
        //while(exchngtokensItr != exchngtokens.end()) {
        //    exchngtokensItr = exchngtokens.erase(exchngtokensItr);
        //} 

        /*
        resultsmnth_index resultsmnth(_code, _code.value);
        auto resultmnthItr = resultsmnth.find(202003);
        eosio_assert(resultmnthItr != resultsmnth.end(), "Does not exist.");
        resultsmnth.erase(resultmnthItr); */

        /*
        dimndhistory_index dimndhistory(_code, _code.value);
        auto iterator = dimndhistory.find(5);
        eosio_assert(iterator != dimndhistory.end(), "DiamondHistory does not exist.");
        dimndhistory.erase(iterator);

        auto iterator2 = dimndhistory.find(6);
        eosio_assert(iterator2 != dimndhistory.end(), "DiamondHistory does not exist.");
        dimndhistory.erase(iterator2);
        */

        /*
        diamondfund_index diamondfund(_self, _self.value);
        auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
        auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
        asset diamondValue = diamondFundIterator->diamondValue;
        uint64_t diamondPkey = diamondFundIterator->pkey; 
        eosio_assert(diamondFundIterator->foundTimestamp == 0, "Diamond is already found.");


        name settingTestName = "testtesttest"_n;
        settings_index settings(_code, _code.value);
        auto iterator = settings.find(settingTestName.value);
        eosio_assert(iterator != settings.end(), "Setting not found");
        settings.modify(iterator, _self, [&]( auto& row ) {
            row.stringvalue = "";
            row.uintvalue = diamondPkey; 
            row.assetvalue = diamondValue;
            row.timestamp = now();
        }); */

        //Mark diamond as found
        /*diamondfund_index diamondfund(_code, _code.value);
        auto itr = diamondfund.rbegin(); //Find the last added diamond fund item
        auto iterator = diamondfund.find(itr->pkey);
        diamondfund.modify(iterator, _self, [&]( auto& row ) {
            row.foundTimestamp = now();
            row.foundInTreasurePkey = 0;
            row.foundByaccount = "testtesttest"_n;
        });*/ 

        //Remove diamond fund
        /*
        diamondfund_index diamondfund(_code, _code.value);
        auto itr = diamondfund.begin();
        while(itr != diamondfund.end()) {
            itr = diamondfund.erase(itr);
        }

        //Remove diamond owners
        diamondownrs_index diamondownrs(_self, _self.value);
        auto downersItr = diamondownrs.begin();
        while(downersItr != diamondownrs.end()) {
            downersItr = diamondownrs.erase(downersItr);
        }

        //Remove diamond payout rows
        payoutdmndow_index payoutdmndow(_self, _self.value);
        auto payoutItr = payoutdmndow.begin();
        while(payoutItr != payoutdmndow.end()) {
            payoutItr = payoutdmndow.erase(payoutItr);
        } 

        //Add diamondfund
        //diamondfund_index diamondfund(_code, _code.value);
        diamondfund.emplace(_self, [&]( auto& row ) { 
            row.pkey = diamondfund.available_primary_key();
            row.toDiamondOwners = eosio::asset(680529, symbol(symbol_code("EOS"), 4));
            row.toTokenHolders = eosio::asset(58296, symbol(symbol_code("EOS"), 4));
            row.diamondValue = eosio::asset(2399011, symbol(symbol_code("EOS"), 4));
            row.foundTimestamp = 0;
        });

        //Copy all diamond owners to diamondownrs
        tcrfund_index tcrfund(_self, _self.value);
        //diamondownrs_index diamondownrs(_self, _self.value);
        for (auto itr = tcrfund.begin(); itr != tcrfund.end(); itr++) {
            diamondownrs.emplace(_self, [&]( auto& row ) {
                row.pkey = diamondownrs.available_primary_key();
                row.account = itr->account;
                row.investedamount = itr->investedamount;
                row.investedpercent = 0; //Null by default. Will be recalculated later
                row.earnedpayout = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                row.timestamp = itr->timestamp;
            });
        }*/

        
        /*diamondfund_index diamondfund(_code, _code.value);
        auto itr = diamondfund.rbegin(); //Find the last added diamond fund item
        auto iterator = diamondfund.find(itr->pkey);
        diamondfund.modify(iterator, _self, [&]( auto& row ) {
            row.diamondValue = eosio::asset(2380114, symbol(symbol_code("EOS"), 4));
            row.toDiamondOwners = eosio::asset(678182, symbol(symbol_code("EOS"), 4));
            row.toTokenHolders = eosio::asset(57556, symbol(symbol_code("EOS"), 4));
        }); */

        //name diamondOwner = "adventurelov"_n;

        /*
        name settingTestName = "testtesttest"_n;
        settings_index settings(_code, _code.value);
        auto iterator = settings.find(settingTestName.value);
        eosio_assert(iterator != settings.end(), "Setting not found");
        settings.modify(iterator, _self, [&]( auto& row ) {
            row.stringvalue = debugInfo;
            row.uintvalue = fromPkey; //redyforpyoutItr->pkey;
            row.timestamp = now();
        }); */


        //Insert or update invested amount for diamond owner.
        /*diamondownrs_index diamondownrs(_self, _self.value);
        auto account_index = diamondownrs.get_index<name("account")>();
        auto diamondownrsItr = account_index.find(diamondOwner.value);
        if(diamondownrsItr == account_index.end()){
            diamondownrs.emplace(_self, [&]( auto& row ) {
                row.pkey = diamondownrs.available_primary_key();
                row.account = diamondOwner;
                row.investedamount = investedAmount;
                row.investedpercent = 0; //Null by default. Will be recalculated later
                row.earnedpayout = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                row.timestamp = now();
            });
        }
        else{
            account_index.modify(diamondownrsItr, _self, [&]( auto& row ) {
                row.investedamount += eosio::asset(0, symbol(symbol_code("EOS"), 4));
            });            
        }*/

        /*
        uint64_t pkey;
        eosio::name account;
        eosio::asset investedamount;
        double investedpercent;
        eosio::asset earnedpayout;
        int32_t timestamp;
        */
        
        //Remove test record
        /*
        diamondfund_index diamondfund(_code, _code.value);
        auto iterator = diamondfund.find(0);
        diamondfund.erase(iterator);

        iterator = diamondfund.find(1);
        diamondfund.erase(iterator);
        */


        /*
        diamondfund_index diamondfund(_code, _code.value);
        auto itr = diamondfund.rbegin(); //Find the last added diamond fund item
        auto iterator = diamondfund.find(itr->pkey);
        diamondfund.modify(iterator, _self, [&]( auto& row ) {
            row.foundTimestamp = now();
            row.foundInTreasurePkey = 1;
            row.foundByaccount = ;
        });

        diamondfund_index diamondfund(_code, _code.value);
        diamondfund.emplace(_self, [&]( auto& row ) { 
            row.pkey = diamondfund.available_primary_key();
            row.toDiamondOwners = eosio::asset(0, symbol(symbol_code("EOS"), 4));
            row.toTokenHolders = eosio::asset(0, symbol(symbol_code("EOS"), 4));
            row.diamondValue = eosio::asset(0, symbol(symbol_code("EOS"), 4));
            row.foundTimestamp = 0;
        });
        */ 

        /*diamondfund_index diamondfund(_code, _code.value);
        auto itr = diamondfund.rbegin(); //Find the last added diamond fund item
        auto iterator = diamondfund.find(itr->pkey);
        diamondfund.modify(iterator, _self, [&]( auto& row ) {
            row.toDiamondOwners = eosio::asset(10, symbol(symbol_code("EOS"), 4));
            row.toTokenHolders = eosio::asset(1, symbol(symbol_code("EOS"), 4));
            row.diamondValue = eosio::asset(1, symbol(symbol_code("EOS"), 4));
        }); */ 
        
        /*
        diamondfund_index diamondfund(_code, _code.value);
        diamondfund.emplace(_self, [&]( auto& row ) { 
            row.pkey = diamondfund.available_primary_key();
            row.toDiamondOwners = eosio::asset(0, symbol(symbol_code("EOS"), 4));
            row.toTokenHolders = eosio::asset(0, symbol(symbol_code("EOS"), 4));
            row.diamondValue = eosio::asset(0, symbol(symbol_code("EOS"), 4));
            row.foundTimestamp = 0;
        }); */
    }

    [[eosio::action]]
    void calcdmndprov(uint64_t fromPkey, name batchName) {
        require_auth("cptblackbill"_n);

        //Find and get info about the current diamond
        diamondfund_index diamondfund(_code, _code.value);
        auto lastAddedDiamondItr = diamondfund.rbegin(); //Find the last added diamond fund item
        auto dmndFundItr = diamondfund.find(lastAddedDiamondItr->pkey);
        eosio_assert(dmndFundItr != diamondfund.end(), "No active diamond found");
        
        //If the diamond is found - then mark batch as redyforpyout. This will be the final
        //calculation of provision that is sent to the payout table 
        if(dmndFundItr->foundTimestamp > 0){
            batchName = "redyforpyout"_n;   
        }
        double diamondValue = dmndFundItr->diamondValue.amount;
        double toDiamondOwners = dmndFundItr->toDiamondOwners.amount;

        //Update investor percent and earned provision. 100 updates for each execute.
        diamondownrs_index diamondownrs(_code, _code.value);
        auto startItr = diamondownrs.lower_bound(fromPkey);
        auto endItr = diamondownrs.upper_bound(fromPkey + 99);
        for (auto itr = startItr; itr != endItr; itr++) {
            double newinvestorpercent = 100;
            double earnedProvision = 0;
            uint64_t earnedProvisionInEos = 0;
            if(diamondValue > 0){
                newinvestorpercent = 100 * itr->investedamount.amount / diamondValue; 
                earnedProvision = (toDiamondOwners * (newinvestorpercent / 100));
                earnedProvisionInEos = earnedProvision;
            }
            
            diamondownrs.modify(itr, _self, [&]( auto& row ) {
                row.investedpercent = newinvestorpercent;
                row.earnedpayout = eosio::asset(earnedProvisionInEos, symbol(symbol_code("EOS"), 4));
                row.batchname = batchName;
            }); 
        }
    }

    [[eosio::action]]
    void prepdmndprov() {
        require_auth("cptblackbill"_n);

        //Check that the current diamond is found
        diamondfund_index diamondfund(_code, _code.value);
        auto lastAddedDiamondItr = diamondfund.rbegin(); //Find the last added diamond fund item
        auto dmndFundItr = diamondfund.find(lastAddedDiamondItr->pkey);
        eosio_assert(dmndFundItr->foundTimestamp > 0, "The current diamond has not been found. Payout preparation for diamond owners is not possible.");
        eosio::asset toTokenHolders = dmndFundItr->toTokenHolders;

        //Check that all rows are marked as redyforpyout
        uint64_t numberOfDiamondOwners = 0;
        uint64_t numberOfReadyForPayoutRows = 0;
        diamondownrs_index diamondownrs(_code, _code.value);
        for (auto itr = diamondownrs.begin(); itr != diamondownrs.end(); itr++) {
            numberOfDiamondOwners++;
            if(itr->batchname == "redyforpyout"_n)
                numberOfReadyForPayoutRows++;      
        }
        eosio_assert(numberOfDiamondOwners == numberOfReadyForPayoutRows, "Not ready for payout preparation. Missing calculated provision for diamond owners."); 

        auto itr = diamondownrs.begin();
        uint64_t counter = 0;
        while(itr != diamondownrs.end()) {
            payoutdmndow_index payoutdmndow(_self, _self.value);
            auto payoutAccountItr = payoutdmndow.find(itr->account.value);
            if(payoutAccountItr == payoutdmndow.end()){
                payoutdmndow.emplace(_self, [&]( auto& row ) {
                    row.account = itr->account;
                    row.payoutamount = itr->earnedpayout;
                    row.memo = "Diamond Owner Provision"; 
                });
            }
            else{
                payoutdmndow.modify(payoutAccountItr, _self, [&]( auto& row ) {
                    row.payoutamount += itr->earnedpayout;
                });            
            }
            
            itr = diamondownrs.erase(itr);

            counter++;
            if(counter >= 99)
                break;
        }

        //Check if all diamond owners rows are prepared and deleted. Then add a new diamond with zero value
        //and add payout amount to token holders (use cptblackbill account until percent pr token holders is calculated)
        if(itr == diamondownrs.end()){
            diamondfund_index diamondfund(_code, _code.value);
            diamondfund.emplace(_self, [&]( auto& row ) { 
                row.pkey = diamondfund.available_primary_key();
                row.toDiamondOwners = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                row.toTokenHolders = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                row.diamondValue = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                row.foundTimestamp = 0;
            });

            name accountCptBlackBill = "redyforpyout"_n; 
            payouttokenh_index payouttokenh(_self, _self.value);
            auto cptblackbillAccountItr = payouttokenh.find(accountCptBlackBill.value);
            if(cptblackbillAccountItr == payouttokenh.end()){
                payouttokenh.emplace(_self, [&]( auto& row ) { 
                    row.account = "cptblackbill"_n;
                    row.payoutamount = toTokenHolders;
                    row.memo = "";
                });
            }
            else{
                payouttokenh.modify(cptblackbillAccountItr, _self, [&]( auto& row ) {
                    row.payoutamount += toTokenHolders;
                });   
            }
        }
    }

    [[eosio::action]]
    void payout(name toAccount) {
        require_auth("cptblackbill"_n);

        payoutdmndow_index payoutdmndow(_code, _code.value);
        auto itr = payoutdmndow.begin();
        uint64_t counter = 0;
        while(itr != payoutdmndow.end()) {
            
            //Send payment to account: itr->account  Amount: itr->payoutamount   Memo: itr->memo
            if(itr->payoutamount.amount > 0 && itr->account != "cptblackbill"_n) 
            {
                //action(
                //    permission_level{ get_self(), "active"_n },
                //    "eosio.token"_n, "transfer"_n,
                //    std::make_tuple(get_self(), itr->account, itr->payoutamount, std::string("Lost Diamond Owner Provision."))
                //).send();

                itr = payoutdmndow.erase(itr);
            }
            
            counter++;
            if(counter > 10)
                break;
        }
    }

    [[eosio::action]]
    void modexpdate(name user, uint64_t pkey) {
        require_auth("cptblackbill"_n); //"Updating expiration date is only allowed by CptBlackBill. This is to make sure (verified gps location by CptBlackBill) that the owner has actually been on location and entered secret code
        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(pkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        
        treasures.modify(iterator, user, [&]( auto& row ) {
            row.expirationdate = now() + 94608000; //Treasure ownership renewed for three years
        });
    }

    [[eosio::action]]
    void resetsecretc(name user, uint64_t pkey) {
        require_auth(user);
        
        treasure_index treasures(_code, _code.value);
        
        auto iterator = treasures.find(pkey);
        eosio_assert(iterator != treasures.end(), "Treasure does not exist.");
        eosio_assert(user == iterator->owner || user == iterator->conqueredby, "You don't have access to reset the secret code on this treasure.");
        eosio_assert(iterator->status == "active", "Treasure is not active.");
        
        //No action requiered since secret code are encryptet somewhere else. Just make sure it's the owner who reset
        //Else this method will fail.    
    }

    [[eosio::action]]
    void erasetreasur(name user, uint64_t pkey) {
        require_auth(user);
        
        treasure_index treasures(_code, _code.value);
        
        auto iterator = treasures.find(pkey);
        eosio_assert(iterator != treasures.end(), "Treasure does not exist.");
        eosio_assert(user == iterator->owner || user == "cptblackbill"_n, "You don't have access to remove this treasure.");
        treasures.erase(iterator);
    }

    [[eosio::action]]
    void erasesellord(name user, uint64_t pkey) {
        require_auth(user);
        
        exchngtokens_index exchngtokens(_code, _code.value);
        
        auto iterator = exchngtokens.find(pkey);
        eosio_assert(iterator != exchngtokens.end(), "Sell order does not exist.");
        eosio_assert(user == iterator->account, "You don't have access to cancel this sell order.");
        eosio_assert(iterator->sell.symbol == symbol(symbol_code("BLKBILL"), 4), "Only sell orders for BLKBILL tokens can be cancelled.");
        eosio_assert(iterator->sell.amount >= 0, "Only sell orders with real quantity can be cancelled.");

        //Transfer BLKBILL tokens back to account
            action(
                permission_level{ get_self(), "active"_n },
                "cptblackbill"_n, "transfer"_n,
                std::make_tuple(get_self(), 
                                iterator->account,  
                                iterator->sell, 
                                std::string("Returned BLKBILL tokens from cancelled sell order."))
            ).send();

        //iterator->sell
        
        exchngtokens.erase(iterator);
    }

    [[eosio::action]]
    void addlike(name account, uint32_t timelineid) 
    {
        require_auth(account);
        
        timelinelike_index timelinelike(_code, _code.value);
        
        bool hasLiked = false;
        uint64_t pkey = 0;
        auto timelineidIdx = timelinelike.get_index<name("timelineid")>();
        auto timelineidItr = timelineidIdx.lower_bound(timelineid); //find(timelineid);
        while(timelineidItr != timelineidIdx.end()){
            if(timelineidItr->account == account && timelineidItr->timelineid == timelineid){
                hasLiked = true;
                pkey = timelineidItr->pkey;
                break;
            }

            if(timelineidItr->timelineid > timelineid)
                break;

            timelineidItr++;
        }

        if(hasLiked){
            //Remove user's like from blockchain
            auto iterator = timelinelike.find(pkey);
            eosio_assert(iterator != timelinelike.end(), "Like does not exist");
            timelinelike.erase(iterator);
        }
        else{
            //Add like
            timelinelike.emplace(_self, [&]( auto& row ) { 
                row.pkey = timelinelike.available_primary_key();
                row.account = account;
                row.timelineid = timelineid;
                row.timestamp = now();
            });
        }
    }

    [[eosio::action]]
    void eraselike(name account, uint32_t timelineid) {
        require_auth(account);

        timelinelike_index timelinelike(_code, _code.value);

        uint64_t pkey = 0;
        auto timelineidIdx = timelinelike.get_index<name("timelineid")>();
        auto timelineidItr = timelineidIdx.lower_bound(timelineid); // find(timelineid);
        while(timelineidItr != timelineidIdx.end()){
            if(timelineidItr->account == account && timelineidItr->timelineid == timelineid){
                pkey = timelineidItr->pkey;
                break;
            }

            if(timelineidItr->timelineid > timelineid)
                break;

            timelineidItr++;
        }

        eosio_assert(pkey > 0, "Like id not found.");

        if(pkey > 0){
            auto iterator = timelinelike.find(pkey);
            eosio_assert(iterator != timelinelike.end(), "Like does not exist");
            timelinelike.erase(iterator);
        }
    }

    [[eosio::action]]
    void addracerslt(eosio::name teamaccount, uint64_t racepkey, std::string checkpointname, uint32_t points, 
                     double latitude, double longitude, eosio::name checkpointcreator, uint32_t totalPoints) 
    {
        require_auth(teamaccount);
        
        eosio_assert(points > 0, "Must have points for storing results.");
        
        raceresults_index raceresults(_code, _code.value);
        raceresults.emplace(_self, [&]( auto& row ) {
            row.pkey = raceresults.available_primary_key();
            row.teamaccount = teamaccount;
            row.racepkey = racepkey;
            row.checkpointname = checkpointname;
            row.points = points;
            row.totalpoints = totalPoints;
            row.latitude = latitude;
            row.longitude = longitude;
            row.creator = checkpointcreator;
            row.timestamp = now();
        });

        //The participants (teamaccount) is racing and race payment can be transfered to race owner, the lost diamond and token holders.
        //racepayments{
        //   row.pkey = racepayments.available_primary_key();
        //   row.racepkey = racePkey;
        //   row.teamaccount = from; //The account who sent money
        //   row.entryfee = eos;
        //   row.feereleased = 0; //Payed, but not sent to race owner (until race is proven to take place with solved checkpoints).
        //   row.eosusdprice = eosusd;
        //   row.timestamp = now();
        //   }
        //Add to diamond fund - TODO: Move this code to where checkpoints are solved.
        //eosio::asset toTokenHolders = (eos * (10 * 100)) / 10000; //10 percent to BLKBILL token holders
        //eosio::asset toDiamondValue = (eos * (10 * 100)) / 10000; //10 percent to diamond value
        //eosio::asset toRaceOwner = (eos * (80 * 100)) / 10000; //80 percent to race owner
        //diamondfund_index diamondfund(_self, _self.value);
        //auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
        //auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
        //diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
        //    row.toTokenHolders += toTokenHolders; //10%
        //    row.diamondValue += toDiamondValue; //10%
        //});

        //TODO Transfer 80% of race fee to race owner  
    }

    [[eosio::action]]
    void clearacerslt() 
    {
        require_auth("cptblackbill"_n);
        
        //Remove race results older than 24 hours
        raceresults_index raceresults(_self, _self.value);
        auto raceresultsItr = raceresults.begin();
        uint64_t counter = 0;
        while(raceresultsItr != raceresults.end()) {
            if(raceresultsItr->timestamp < (now() - 86400)){ //Timestamp older than 24 hours
                raceresultsItr = raceresults.erase(raceresultsItr);
            } 

            //Prevent deadline exceeded error
            counter++;
            if(counter > 500){
                break;
            }
        }
    }

    [[eosio::action]]
    void delracersult(uint64_t raceId, eosio::name teamaccount) {
        require_auth("cptblackbill"_n);
        
        raceresults_index raceresults(_self, _self.value);
        auto itr = raceresults.begin();
        while(itr != raceresults.end()) {
            itr = raceresults.erase(itr);
        } 
    }

    [[eosio::action]]
    void addsetting(name keyname, std::string stringvalue, asset assetvalue, uint32_t uintvalue) 
    {
        require_auth("cptblackbill"_n);
        
        settings_index settings(_code, _code.value);
        
        settings.emplace(_self, [&]( auto& row ) { //The user who run the transaction is RAM payer. So if added from CptBlackBill dapp, CptBlackBill is responsible for RAM.
            row.keyname = keyname; // pkey = settings.available_primary_key();
            //row.key = key; 
            row.stringvalue = stringvalue;
            row.assetvalue = assetvalue;
            row.uintvalue = uintvalue;
            row.timestamp = now();
        });
    }
    
    [[eosio::action]]
    void modsetting(name keyname, std::string stringvalue, asset assetvalue, uint32_t uintvalue) 
    {
        require_auth("cptblackbill"_n);
        settings_index settings(_code, _code.value);
        auto iterator = settings.find(keyname.value);
        eosio_assert(iterator != settings.end(), "Setting not found");
        
        settings.modify(iterator, _self, [&]( auto& row ) {
            row.stringvalue = stringvalue;
            row.assetvalue = assetvalue;
            row.uintvalue = uintvalue;
            row.timestamp = now();
        });
    }

    [[eosio::action]]
    void erasesetting(name keyname) {
        require_auth("cptblackbill"_n);
        
        settings_index settings(_code, _code.value);
        auto iterator = settings.find(keyname.value);
        eosio_assert(iterator != settings.end(), "Setting does not exist");
        settings.erase(iterator);
    }

    [[eosio::action]]
    void eraseresult(uint64_t pkey) {
        require_auth("cptblackbill"_n);
        
        results_index results(_code, _code.value);
        auto iterator = results.find(pkey);
        eosio_assert(iterator != results.end(), "Result does not exist.");
        results.erase(iterator);
    }

    [[eosio::action]]
    void erasetcrf(name account) {
        require_auth("cptblackbill"_n);
        
        tcrfund_index tcrfund(_code, _code.value);
        auto iterator = tcrfund.find(account.value);
        eosio_assert(iterator != tcrfund.end(), "Tcrf-account does not exist.");
        tcrfund.erase(iterator);
    }

    [[eosio::action]]
    void exechestfnd(uint64_t pkey) {
        require_auth("cptblackbill"_n);
        
        rndchestfnd_index rndchestfnd(_code, _code.value);
        auto iterator = rndchestfnd.find(pkey);
        eosio_assert(iterator != rndchestfnd.end(), "Chest funding not found");
        
        rndchestfnd.modify(iterator, _self, [&]( auto& row ) {
            row.executed = true;
        });
    }

    [[eosio::action]]
    void modrace(eosio::name raceowner, uint64_t racepkey, std::string title, asset entryfeeusd, std::string jsonracedata) 
    {
        require_auth(raceowner);
        
        race_index race(_code, _code.value);
        auto iterator = race.find(racepkey);
        eosio_assert(iterator != race.end(), "Race not found");
        
        race.modify(iterator, _self, [&]( auto& row ) {
            row.title = title;
            row.entryfeeusd = entryfeeusd;
            row.racedata = jsonracedata;
        });
    }

    [[eosio::action]]
    void upsertcrew(name user, name crewmember, std::string imagehash, std::string quote) 
    {
        require_auth( user );
        crewinfo_index crewinfo(_code, _code.value);
        auto iterator = crewinfo.find(crewmember.value);
        if( iterator == crewinfo.end() )
        {
            eosio_assert(user == crewmember || user == "cptblackbill"_n, "Only Cpt.BlackBill can insert crewmembers on behalf of other users.");
            crewinfo.emplace(user, [&]( auto& row ) {
                row.user = crewmember;
                row.imagehash = imagehash;
                row.quote = quote;
            });
        }
        else {
            eosio_assert(user == iterator->user || user == "cptblackbill"_n, "You don't have access to modify this crewmember.");
            crewinfo.modify(iterator, user, [&]( auto& row ) {
                row.imagehash = imagehash;
                row.quote = quote;
            });
        }
    }

    [[eosio::action]]
    void erasecrew(name user) {
        require_auth( user );
        
        crewinfo_index crewinfo(_code, _code.value);
        auto iterator = crewinfo.find(user.value);
        eosio_assert(iterator != crewinfo.end(), "Crew-info does not exist.");
        crewinfo.erase(iterator);
    }

    [[eosio::action]]
    void adddimndhst(uint64_t treasurepkey, asset diamondValueInEos, asset diamondValueInUsd, int32_t fromTimestamp, int32_t toTimestamp)
    {
        require_auth("cptblackbill"_n);

        eosio_assert(treasurepkey >= 0, "Invalid treasure pKey.");
        
        dimndhistory_index dimndhistory(_code, _code.value);
        
        dimndhistory.emplace(_self, [&]( auto& row ) {
            row.pkey = dimndhistory.available_primary_key();
            row.treasurepkey = treasurepkey;
            row.diamondValueInEos = diamondValueInEos;
            row.diamondValueInUsd = diamondValueInUsd;
            row.fromTimestamp = fromTimestamp;
            row.toTimestamp = toTimestamp;
        });
        
        //Remove 2% of the diamond value. That amount is added to a random treasure in the RelocateTheLostDiamond function
        diamondfund_index diamondfund(_self, _self.value);
        auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
        auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
        asset diamondValue = diamondFundIterator->diamondValue;
        
        double dblToRandomTreasure = (diamondValue.amount * 1.0) / 100; //1.0 percent (is actually 2 percent of the diamond value)
        uint64_t intToRandomTreasure = dblToRandomTreasure;
        uint64_t intRemainingDiamondValue = diamondValue.amount - intToRandomTreasure; 

        //Update new amount for diamond value
        asset eosRemainingDiamondValue = eosio::asset(intRemainingDiamondValue, symbol(symbol_code("EOS"), 4));
        diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
            row.diamondValue = eosRemainingDiamondValue;
        });
    }

    [[eosio::action]]
    void addsponsitm(std::string sponsorname, std::string imageurl, std::string description,
                     std::string targeturl, asset usdvalue, asset adFeePrice) 
    {
        require_auth("cptblackbill"_n);

        eosio_assert(imageurl.length() <= 125, "Max length of imageUrl is 125 characters.");
        eosio_assert(description.length() <= 250, "Max length of description is 250 characters.");
        eosio_assert(targeturl.length() <= 125, "Max length of targeturl is 125 characters.");
        eosio_assert(usdvalue.amount >= 1000, "Minimum USD value for sponsored item is 10 dollar.");
        eosio_assert(adFeePrice.amount >= 10, "Minimum EOS value for advertising fee is 0.0010 EOS.");

        sponsoritems_index sponsoritems(_code, _code.value);
        sponsoritems.emplace(_self, [&]( auto& row ) {
            row.pkey = sponsoritems.available_primary_key();
            row.sponsorname = sponsorname;
            row.imageurl = imageurl;
            row.description = description;
            row.targeturl = targeturl;
            row.usdvalue = usdvalue;
            row.adFeePrice = adFeePrice;
            row.status = "pendingforadfeepayment";
            row.treasurepkey = 0;
            row.timestamp = now();
        });
    }

    [[eosio::action]]
    void erasesponitm(uint64_t pkey) {
        require_auth("cptblackbill"_n);
        
        sponsoritems_index sponsoritems(_code, _code.value);
        
        auto iterator = sponsoritems.find(pkey);
        eosio_assert(iterator != sponsoritems.end(), "Sponsor item does not exist.");
        sponsoritems.erase(iterator);
    }

    /*[[eosio::action]]
    void runpayout(name user) {
        require_auth("cptbbpayout1"_n);
    }*/

private:
    struct [[eosio::table]] account {
        asset    balance;
        uint64_t primary_key()const { return balance.symbol.code().raw(); }
    };

    struct [[eosio::table]] currency_stats {
        asset    supply;
        asset    max_supply;
        name     issuer;
        uint64_t primary_key()const { return supply.symbol.code().raw(); }
    };

    typedef eosio::multi_index< "accounts"_n, account > accounts;
    typedef eosio::multi_index< "stat"_n, currency_stats > stats;

    struct [[eosio::table]] treasure {
        uint64_t pkey;
        eosio::name owner;
        std::string title; 
        std::string description;
        std::string imageurl;
        std::string treasuremapurl;
        std::string videourl; //Link to video (Must be a video provider that support API to views and likes)
        double latitude; //GPS coordinate
        double longitude; //GPS coordinate
        double tileidxy; //Map tile id by x,y. Zoom level 17
        uint64_t rankingpoint; //Calculated and updated by CptBlackBill based on video and turnover stats.  
        int32_t timestamp; //Date created
        int32_t expirationdate; //Date when ownership expires - other users can then take ownnership of this treasure location
        std::string secretcode;
        std::string status;
        uint64_t banditalarms; 
        uint64_t noOfCaptures;
        uint64_t ctypeid; //Type of checkpoint
        eosio::name conqueredby; //If someone has robbed and conquered the treasure. Conquered by user will get 75% of the treasure value next time it's robbed. The owner will still get 25%
        std::string conqueredimg; //The user who conquered can add another image to the treasure.
        std::string jsondata;  //additional field for other info in json format.
        uint64_t primary_key() const { return  pkey; }
        uint64_t by_owner() const {return owner.value; } //second key, can be non-unique
        double by_latitude() const {return latitude; } //third key, can be non-unique
        double by_tileid() const {return tileidxy; } //fourth key, can be non-unique
        uint64_t by_ctypeid() const {return ctypeid; } //fifth key, can be non-unique
    };
    typedef eosio::multi_index<"treasure"_n, treasure,  
            eosio::indexed_by<"owner"_n, const_mem_fun<treasure, uint64_t, &treasure::by_owner>>,
            eosio::indexed_by<"latitude"_n, const_mem_fun<treasure, double, &treasure::by_latitude>>,
            eosio::indexed_by<"tileidxy"_n, const_mem_fun<treasure, double, &treasure::by_tileid>>,
            eosio::indexed_by<"ctypeid"_n, const_mem_fun<treasure, uint64_t, &treasure::by_ctypeid>>> treasure_index;

    struct [[eosio::table]] checkpoint {
        uint64_t pkey;
        eosio::name owner;
        std::string title; 
        std::string description;
        std::string imageurl;
        std::string treasuremapurl;
        std::string videourl; //Link to video (Must be a video provider that support API to views and likes)
        double latitude; //GPS coordinate
        double longitude; //GPS coordinate
        double tileidxy; //Map tile id by x,y. Zoom level 17
        uint64_t rankingpoint; //Calculated and updated by CptBlackBill based on video and turnover stats.  
        int32_t timestamp; //Date created
        int32_t expirationdate; //Date when ownership expires - other users can then take ownnership of this treasure location
        std::string secretcode;
        std::string status;
        uint64_t banditalarms; 
        uint64_t noOfCaptures;
        uint64_t ctypeid; //Type of checkpoint
        eosio::name conqueredby; //If someone has robbed and conquered the treasure. Conquered by user will get 75% of the treasure value next time it's robbed. The owner will still get 25%
        std::string conqueredimg; //The user who conquered can add another image to the treasure.
        std::string jsondata;  //additional field for other info in json format.
        uint64_t primary_key() const { return  pkey; }
        uint64_t by_owner() const {return owner.value; } //second key, can be non-unique
        double by_latitude() const {return latitude; } //third key, can be non-unique
        double by_tileid() const {return tileidxy; } //fourth key, can be non-unique
        uint64_t by_ctypeid() const {return ctypeid; } //fifth key, can be non-unique
    };
    typedef eosio::multi_index<"checkpoint"_n, checkpoint, 
            eosio::indexed_by<"owner"_n, const_mem_fun<checkpoint, uint64_t, &checkpoint::by_owner>>,
            eosio::indexed_by<"latitude"_n, const_mem_fun<checkpoint, double, &checkpoint::by_latitude>>,
            eosio::indexed_by<"tileidxy"_n, const_mem_fun<checkpoint, double, &checkpoint::by_tileid>>,
            eosio::indexed_by<"ctypeid"_n, const_mem_fun<checkpoint, uint64_t, &checkpoint::by_ctypeid>>> checkpoint_index;

    struct [[eosio::table]] tcrfund {
        eosio::name account;
        eosio::asset investedamount;
        double investorpercent;
        eosio::asset earnedpayout;
        int32_t timestamp;

        uint64_t primary_key() const { return  account.value; }
    };
    typedef eosio::multi_index<"tcrfund"_n, tcrfund> tcrfund_index;
    
    struct [[eosio::table]] diamondfund {
        uint64_t pkey;
        eosio::asset toDiamondOwners;
        eosio::asset toTokenHolders;
        eosio::asset diamondValue;
        std::string memo;
        int32_t foundTimestamp;
        uint64_t foundInTreasurePkey;
        eosio::name foundByaccount;

        uint64_t primary_key() const { return  pkey; }
    };
    typedef eosio::multi_index<"diamondfund"_n, diamondfund> diamondfund_index;

    struct [[eosio::table]] diamondownrs {
        uint64_t pkey;
        eosio::name account;
        eosio::asset investedamount;
        double investedpercent;
        eosio::asset earnedpayout;
        int32_t timestamp;
        eosio::name batchname;

        uint64_t primary_key() const { return  pkey; }
        uint64_t by_account() const {return account.value; } //second key, can be non-unique
        uint64_t by_batchname() const {return batchname.value; } //third key, can be non-unique
    };
    typedef eosio::multi_index<"diamondownrs"_n, diamondownrs, 
            eosio::indexed_by<"account"_n, const_mem_fun<diamondownrs, uint64_t, &diamondownrs::by_account>>,
            eosio::indexed_by<"batchname"_n, const_mem_fun<diamondownrs, uint64_t, &diamondownrs::by_batchname>>> diamondownrs_index;

    struct [[eosio::table]] payoutdmndow { //Payout table for diamond owners
        eosio::name account;
        eosio::asset payoutamount;
        std::string memo;

        uint64_t primary_key() const { return  account.value; }
    };
    typedef eosio::multi_index<"payoutdmndow"_n, payoutdmndow> payoutdmndow_index;

    struct [[eosio::table]] payouttokenh { //Payout table for token holders
        eosio::name account;
        eosio::asset payoutamount;
        std::string memo;

        uint64_t primary_key() const { return  account.value; }
    };
    typedef eosio::multi_index<"payouttokenh"_n, payouttokenh> payouttokenh_index;

    struct [[eosio::table]] rndchestfnd {
        uint64_t pkey;
        eosio::name from;
        eosio::asset amount;
        std::string memo;
        bool executed;
        int32_t timestamp; 
        
        uint64_t primary_key() const { return  pkey; }
    };
    typedef eosio::multi_index<"rndchestfnd"_n, rndchestfnd> rndchestfnd_index;
    
    struct [[eosio::table]] settings {
        eosio::name keyname; 
        std::string stringvalue;
        eosio::asset assetvalue;
        uint32_t uintvalue;
        int32_t timestamp; //last updated
        
        uint64_t primary_key() const { return keyname.value; }
    };
    typedef eosio::multi_index<"settings"_n, settings> settings_index;
    
    struct [[eosio::table]] results {
        uint64_t pkey;
        uint64_t treasurepkey;
        eosio::name user;
        eosio::name creator;
        eosio::name conqueredby;
        bool lostdiamondfound;
        eosio::asset payouteos;
        eosio::asset eosusdprice;
        eosio::asset minedblkbills;
        int32_t timestamp; //Date created - queue order
        
        uint64_t primary_key() const { return  pkey; }
        uint64_t by_user() const {return user.value; } //second key, can be non-unique
        uint64_t by_creator() const {return creator.value; } //third key, can be non-unique
        uint64_t by_treasurepkey() const {return treasurepkey; } //fourth key, can be non-unique
    };
    typedef eosio::multi_index<"results"_n, results, 
            eosio::indexed_by<"user"_n, const_mem_fun<results, uint64_t, &results::by_user>>, 
            eosio::indexed_by<"creator"_n, const_mem_fun<results, uint64_t, &results::by_creator>>, 
            eosio::indexed_by<"treasurepkey"_n, const_mem_fun<results, uint64_t, &results::by_treasurepkey>>> results_index;

    
    struct [[eosio::table]] race {
        uint64_t pkey;
        eosio::name raceowner;
        std::string title;
        eosio::asset entryfeeusd;
        std::string racedata;
        int32_t timestamp; //Date created
        
        uint64_t primary_key() const { return  pkey; }
        uint64_t by_raceowner() const {return raceowner.value; } //second key, can be non-unique
    };
    typedef eosio::multi_index<"race"_n, race, 
            eosio::indexed_by<"raceowner"_n, const_mem_fun<race, uint64_t, &race::by_raceowner>>> race_index; 
    
    struct [[eosio::table]] raceresults {
        uint64_t pkey;
        uint64_t racepkey;
        std::string checkpointname;
        int32_t points;
        int32_t totalpoints;
        eosio::name teamaccount;
        eosio::name creator;
        double latitude; //GPS coordinate
        double longitude; //GPS coordinate
        int32_t timestamp; //Date created - queue order
        
        uint64_t primary_key() const { return  pkey; }
        uint64_t by_teamaccount() const {return teamaccount.value; } //second key, can be non-unique
        uint64_t by_creator() const {return creator.value; } //third key, can be non-unique
        uint64_t by_racepkey() const {return racepkey; } //fourth key, can be non-unique
    };
    typedef eosio::multi_index<"raceresults"_n, raceresults, 
            eosio::indexed_by<"teamaccount"_n, const_mem_fun<raceresults, uint64_t, &raceresults::by_teamaccount>>, 
            eosio::indexed_by<"creator"_n, const_mem_fun<raceresults, uint64_t, &raceresults::by_creator>>, 
            eosio::indexed_by<"racepkey"_n, const_mem_fun<raceresults, uint64_t, &raceresults::by_racepkey>>> raceresults_index;

    struct [[eosio::table]] racepayments {
        uint64_t pkey;
        uint64_t racepkey;
        eosio::name teamaccount;
        eosio::asset entryfee;
        bool feereleased; //0 = Entry fee is payed, but not sent to race owner. 1 = Entry fee is sent to race owner on first checkpoint solved (or if other race participants solved checkpoints)
        eosio::asset eosusdprice;
        int32_t timestamp; //Date created - queue order
        
        uint64_t primary_key() const { return  pkey; }
        uint64_t by_teamaccount() const {return teamaccount.value; } //second key, can be non-unique
        uint64_t by_racepkey() const {return racepkey; } //third key, can be non-unique
    };
    typedef eosio::multi_index<"racepayments"_n, racepayments, 
            eosio::indexed_by<"teamaccount"_n, const_mem_fun<racepayments, uint64_t, &racepayments::by_teamaccount>>, 
            eosio::indexed_by<"racepkey"_n, const_mem_fun<racepayments, uint64_t, &racepayments::by_racepkey>>> racepayments_index;

    struct [[eosio::table]] resultsmnth {
        uint64_t pkey;
        eosio::name fpAccount;
        int32_t fpPoints; 
        eosio::asset fpEos;
        eosio::name spAccount;
        int32_t spPoints; 
        eosio::asset spEos;
        eosio::name tpAccount;
        int32_t tpPoints; 
        eosio::asset tpEos;
        eosio::asset eosusdprice;
        int32_t timestamp; 
        
        uint64_t primary_key() const { return  pkey; }
    };
    typedef eosio::multi_index<"resultsmnth"_n, resultsmnth> resultsmnth_index;
        
    struct [[eosio::table]] timelinelike {
        uint64_t pkey;
        eosio::name account;
        uint64_t timelineid;
        int32_t timestamp; //Date created 
        
        uint64_t primary_key() const { return  pkey; }
        uint64_t by_account() const {return account.value; } //second key, can be non-unique
        uint64_t by_timelineid() const {return timelineid; } //third key, can be non-unique
    };
    typedef eosio::multi_index<"timelinelike"_n, timelinelike, 
            eosio::indexed_by<"account"_n, const_mem_fun<timelinelike, uint64_t, &timelinelike::by_account>>, 
            eosio::indexed_by<"timelineid"_n, const_mem_fun<timelinelike, uint64_t, &timelinelike::by_timelineid>>> timelinelike_index; 
    
    
    //2021-10-03
    struct [[eosio::table]] treasuresale {
        uint64_t pkey;
        uint64_t treasurepkey;
        eosio::name account;
        eosio::asset askingpriceUsd;
        std::string memo;
        int32_t expirationdate; 
        int32_t timestamp; //Date created 
        
        uint64_t primary_key() const { return  pkey; }
        uint64_t by_account() const {return account.value; } //second key, can be non-unique
        uint64_t by_treasurepkey() const {return treasurepkey; } //third key, can be non-unique
    };
    typedef eosio::multi_index<"treasuresale"_n, treasuresale, 
            eosio::indexed_by<"account"_n, const_mem_fun<treasuresale, uint64_t, &treasuresale::by_account>>, 
            eosio::indexed_by<"treasurepkey"_n, const_mem_fun<treasuresale, uint64_t, &treasuresale::by_treasurepkey>>> treasuresale_index; 
    
    //Struck to show where the lost diamond has been located earlier
    struct [[eosio::table]] dimndhistory {
        uint64_t pkey;
        uint64_t treasurepkey;
        eosio::asset diamondValueInEos;
        eosio::asset diamondValueInUsd;
        int32_t fromTimestamp; 
        int32_t toTimestamp;
        
        uint64_t primary_key() const { return  pkey; }
    };
    typedef eosio::multi_index<"dimndhistory"_n, dimndhistory> dimndhistory_index;

    struct [[eosio::table]] sponsoritems {
        uint64_t pkey;
        std::string sponsorname;
        std::string imageurl;
        std::string description;
        std::string targeturl;
        eosio::asset usdvalue;
        eosio::asset adFeePrice;
        std::string status;
        eosio::name wonby;
        uint64_t treasurepkey;
        int32_t wontimestamp;
        int32_t timestamp; //Date created - queue order
        
        uint64_t primary_key() const { return  pkey; }
    };
    typedef eosio::multi_index<"sponsoritems"_n, sponsoritems> sponsoritems_index;
    
    struct [[eosio::table]] crewinfo {
        eosio::name user;
        std::string imagehash;
        std::string quote;
        
        uint64_t primary_key() const { return  user.value; }
    };
    typedef eosio::multi_index<"crewinfo"_n, crewinfo> crewinfo_index;

    struct [[eosio::table]] teambearland {
        uint64_t pkey;
        eosio::name teamMember;
        std::string youTubeName;
        
        uint64_t primary_key() const { return  pkey; }
    };
    typedef eosio::multi_index<"teambearland"_n, teambearland> teambearland_index;

    
    //2020-05-16 Struck for selling tokens on CptBlackBill exchange
    struct [[eosio::table]] exchngtokens {
        uint64_t pkey;
        eosio::name account;
        eosio::asset sell;
        eosio::asset itemprice;
        int32_t timestamp; //Date created 
        
        uint64_t primary_key() const { return  pkey; }
        uint64_t by_itemprice() const {return itemprice.amount; } //second key, can be non-unique
        uint64_t by_account() const {return account.value; } //third key, can be non-unique
    };
    typedef eosio::multi_index<"exchngtokens"_n, exchngtokens, 
            eosio::indexed_by<"itemprice"_n, const_mem_fun<exchngtokens, uint64_t, &exchngtokens::by_itemprice>>,
            eosio::indexed_by<"account"_n, const_mem_fun<exchngtokens, uint64_t, &exchngtokens::by_account>>> exchngtokens_index; 

    //2020-05-16 Struct for logging exchange buy/sell events on CptBlackBill exchange
    struct [[eosio::table]] exchngbuylog {
        uint64_t pkey;
        eosio::name toaccount;
        eosio::asset tokens;
        eosio::asset itemprice;
        eosio::asset eosprice;
        int32_t timestamp; //Date created 
        
        uint64_t primary_key() const { return  pkey; }
        uint64_t by_toaccount() const {return toaccount.value; } //second key, can be non-unique
    };
    typedef eosio::multi_index<"exchngbuylog"_n, exchngbuylog, 
            eosio::indexed_by<"toaccount"_n, const_mem_fun<exchngbuylog, uint64_t, &exchngbuylog::by_toaccount>>> exchngbuylog_index; 
    
    
    /*void send_summary(name user, std::string message) {
        action(
            permission_level{get_self(),"active"_n},
            get_self(),
            "notify"_n,
            std::make_tuple(user, name{user}.to_string() + message)
        ).send();
    };*/

    //---Get dapp settings---------------------------------------------------------------------------------
    asset getEosUsdPrice() {
        asset eosusd = eosio::asset(0, symbol(symbol_code("USD"), 4)); //default value
        
        //Get settings from table if exists. If not, default value is used
        settings_index settings(_self, _self.value);
        auto iterator = settings.find(name("eosusd").value); 
        if(iterator != settings.end()){
            eosusd = iterator->assetvalue;    
        }
        return eosusd;
    };
    
    asset getPriceInUSD(asset eos) {
        asset eosusd = eosio::asset(27600, symbol(symbol_code("USD"), 4)); //default value
        
        //Get settings from table if exists. If not, default value is used
        settings_index settings(_self, _self.value);
        auto iterator = settings.find(name("eosusd").value); 
        if(iterator != settings.end()){
            eosusd = iterator->assetvalue;    
        }
                 
        uint64_t priceUSD = (eos.amount * eosusd.amount) / 10000;
        return eosio::asset(priceUSD, symbol(symbol_code("USD"), 4));
    };

    asset getPriceForCheckTreasureValueInEOS() {
        asset eosusd = eosio::asset(27600, symbol(symbol_code("USD"), 4)); //default value
        asset priceForCheckingTreasureValueInUSD = eosio::asset(20000, symbol(symbol_code("USD"), 4)); //default value for checking a treasure chest value
        
        //Get settings from table if exists. If not, default value is used
        settings_index settings(_self, _self.value);
        auto iterator = settings.find(name("eosusd").value); 
        if(iterator != settings.end()){
            eosusd = iterator->assetvalue;    
        }
        
        auto iterator2 = settings.find(name("checktreasur").value); 
        if(iterator2 != settings.end()){
            priceForCheckingTreasureValueInUSD = iterator2->assetvalue;    
        }
        
        double dblPriceInEOS = (priceForCheckingTreasureValueInUSD.amount * 10000) / eosusd.amount;
        uint64_t priceInEOS = (uint64_t)dblPriceInEOS;   
        //asset cptbbPrice = eosio::asset(priceInEOS, symbol(symbol_code("EOS"), 4));
        
        //For debugging
        /*settings_index settingsdebug(_self, _self.value);
        auto iterator3 = settingsdebug.find(name("minsponsoraw").value);
        eosio_assert(iterator3 != settingsdebug.end(), "Setting not found2");
        
        settingsdebug.modify(iterator3, _self, [&]( auto& row ) {
            row.stringvalue = "test2";
            row.assetvalue = cptbbPrice; //cptbbPrice;
            row.uintvalue = priceInEOS; //eosusd.amount; //priceInEOS;
            row.timestamp = now();
        }); */
        
        return eosio::asset(priceInEOS, symbol(symbol_code("EOS"), 4));
    };
    //-----------------------------------------------------------------------------------------------------

    static constexpr uint64_t string_to_symbol( uint8_t precision, const char* str ) {
        uint32_t len = 0;
        while( str[len] ) ++len;

        uint64_t result = 0;
        for( uint32_t i = 0; i < len; ++i ) {
            if( str[i] < 'A' || str[i] > 'Z' ) {
                /// ERRORS?
            } else {
                result |= (uint64_t(str[i]) << (8*(1+i)));
            }
        }

        result |= uint64_t(precision);
        return result;
    }
};

//EOSIO_DISPATCH( cptblackbill, (create)(issue)(transfer)(addtreasure)(erasetreasur)(modtreasure)(checktreasur)(modtrchest))

extern "C" {
  void apply(uint64_t receiver, uint64_t code, uint64_t action) {
    auto self = receiver;
    //cptblackbill _cptblackbill(receiver);
    //if(code==receiver && action==name("addtreasure").value) {
    //  execute_action(name(receiver), name(code), &cptblackbill::addtreasure );
    //}
    if(code==receiver && action==name("btulla").value) {
      execute_action(name(receiver), name(code), &cptblackbill::btulla );
    }
    else if(code==receiver && action==name("calcdmndprov").value) {
      execute_action(name(receiver), name(code), &cptblackbill::calcdmndprov );
    }
    else if(code==receiver && action==name("prepdmndprov").value) {
      execute_action(name(receiver), name(code), &cptblackbill::prepdmndprov );
    }
    else if(code==receiver && action==name("payout").value) {
      execute_action(name(receiver), name(code), &cptblackbill::payout );
    }
    else if(code==receiver && action==name("addteammbr").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addteammbr );
    }
    else if(code==receiver && action==name("delteammbr").value) {
      execute_action(name(receiver), name(code), &cptblackbill::delteammbr );
    }
    else if(code==receiver && action==name("addtradmin").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addtradmin );
    }
    else if(code==receiver && action==name("addsellprice").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addsellprice );
    }
    else if(code==receiver && action==name("delsellprice").value) {
      execute_action(name(receiver), name(code), &cptblackbill::delsellprice );
    }
    else if(code==receiver && action==name("airdrop").value) {
      execute_action(name(receiver), name(code), &cptblackbill::airdrop );
    }
    else if(code==receiver && action==name("addlike").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addlike );
    }
    else if(code==receiver && action==name("eraselike").value) {
      execute_action(name(receiver), name(code), &cptblackbill::eraselike );
    }
    else if(code==receiver && action==name("modtreasure").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modtreasure );
    }
    else if(code==receiver && action==name("exechestfnd").value) {
      execute_action(name(receiver), name(code), &cptblackbill::exechestfnd );
    }
    else if(code==receiver && action==name("modrace").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modrace );
    }
    else if(code==receiver && action==name("modtreasimg").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modtreasimg );
    }
    else if(code==receiver && action==name("moddmndval").value) {
      execute_action(name(receiver), name(code), &cptblackbill::moddmndval );
    }
    else if(code==receiver && action==name("modgps").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modgps );
    }
    else if(code==receiver && action==name("modtreasjson").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modtreasjson );
    }
    else if(code==receiver && action==name("modsecretcode").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modsecretcode );
    }
    else if(code==receiver && action==name("activatchest").value) {
      execute_action(name(receiver), name(code), &cptblackbill::activatchest );
    }
    else if(code==receiver && action==name("updranking").value) {
      execute_action(name(receiver), name(code), &cptblackbill::updranking );
    }
    else if(code==receiver && action==name("awardpayout").value) {
      execute_action(name(receiver), name(code), &cptblackbill::awardpayout );
    }
    else if(code==receiver && action==name("modexpdate").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modexpdate );
    }
    else if(code==receiver && action==name("resetsecretc").value) {
      execute_action(name(receiver), name(code), &cptblackbill::resetsecretc );
    }
    else if(code==receiver && action==name("unlockchest").value) {
      execute_action(name(receiver), name(code), &cptblackbill::unlockchest );
    }
    else if(code==receiver && action==name("erasetreasur").value) {
      execute_action(name(receiver), name(code), &cptblackbill::erasetreasur );
    }
    else if(code==receiver && action==name("erasesellord").value) {
      execute_action(name(receiver), name(code), &cptblackbill::erasesellord );
    }
    else if(code==receiver && action==name("addresult").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addresult );
    }
    else if(code==receiver && action==name("addracerslt").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addracerslt );
    }
    else if(code==receiver && action==name("clearacerslt").value) {
      execute_action(name(receiver), name(code), &cptblackbill::clearacerslt );
    }
    else if(code==receiver && action==name("delracersult").value) {
      execute_action(name(receiver), name(code), &cptblackbill::delracersult );
    }
    else if(code==receiver && action==name("addsetting").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addsetting );
    }
    else if(code==receiver && action==name("modsetting").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modsetting );
    }
    else if(code==receiver && action==name("erasesetting").value) {
      execute_action(name(receiver), name(code), &cptblackbill::erasesetting );
    }
    else if(code==receiver && action==name("erasetcrf").value) {
      execute_action(name(receiver), name(code), &cptblackbill::erasetcrf );
    }
    else if(code==receiver && action==name("eraseresult").value) {
      execute_action(name(receiver), name(code), &cptblackbill::eraseresult );
    }
    else if(code==receiver && action==name("upsertcrew").value) {
      execute_action(name(receiver), name(code), &cptblackbill::upsertcrew );
    }
    else if(code==receiver && action==name("erasecrew").value) {
      execute_action(name(receiver), name(code), &cptblackbill::erasecrew );
    }
    else if(code==receiver && action==name("adddimndhst").value) {
      execute_action(name(receiver), name(code), &cptblackbill::adddimndhst );
    }
    else if(code==receiver && action==name("addsponsitm").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addsponsitm );
    }
    else if(code==receiver && action==name("erasesponitm").value) {
      execute_action(name(receiver), name(code), &cptblackbill::erasesponitm );
    }
    else if(code==receiver && action==name("issue").value) {
      execute_action(name(receiver), name(code), &cptblackbill::issue );
    }
    else if(code==receiver && action==name("transfer").value) {
      execute_action(name(receiver), name(code), &cptblackbill::transfer );
    }
    else if(code==name("eosio.token").value && action==name("transfer").value) {
      execute_action(name(receiver), name(code), &cptblackbill::onTransfer );
    }
  }
};

