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

            //Take 25 percent of the transfered EOS as provision to the lost diamond owners
            eosio::asset totcrfund = (eos * (25 * 100)) / 10000;
            tcrfund_index tcrfund(_self, _self.value);
            //Update investors earnedpayout base on each investors percent
            for (auto itr = tcrfund.begin(); itr != tcrfund.end(); itr++) {
                eosio::asset earnedpayout = (totcrfund * (itr->investorpercent * 100)) / 10000;
                tcrfund.modify(itr, _self, [&]( auto& row ) {
                    row.earnedpayout = row.earnedpayout + earnedpayout;
                }); 
            }

            //Add 20 percent to the value of the Lost Diamond (account cptblackbill is used for this)
            //The provision earned to account cptblackbill is transfered to a random treasure when the lost diamond is found
            eosio::asset toLostDiamondValueByCptBlackBill = (eos * (20 * 100)) / 10000;
            auto existingiterator2 = tcrfund.find(to.value); //to = cptblackbill
            tcrfund.modify(existingiterator2, _self, [&]( auto& row ) {
                row.investedamount = row.investedamount + toLostDiamondValueByCptBlackBill;
            });

            //The remaining 5 percent is accumulated on the cptblackbill account and used for cpu/net resources
            //and payout to BLKBILL token holders (See the Admin Statistics page.)

            //Add row to verifycheck
            verifycheck_index verifycheck(_self, _self.value);
            verifycheck.emplace(_self, [&]( auto& row ) {
                row.pkey = verifycheck.available_primary_key();
                row.treasurepkey = treasurepkey;
                row.byaccount = from;
                row.addtochestamount = eos - (2 * totcrfund); //Amount paid - 25 percent provision to diamond owners and 25% to diamond value
                row.timestamp = now();
            });
        }
        else if (memo.rfind("Unlock Treasure No.", 0) == 0) {
            //from account pays to unlock a treasure

            eosio_assert(eos >= getPriceForCheckTreasureValueInEOS(), "Transfered amount is below minimum price for unlocking a treasure.");
            
            //Get treasurepkey and secret code from memo
            replace(memo, "Unlock Treasure No.", "");
            std::size_t delimiterlocation = memo.find("-");
            uint64_t treasurepkey = std::strtoull( memo.substr(0, delimiterlocation).c_str(),NULL,0 ); 
            std::string secretcode = memo.substr(delimiterlocation + 1);

            treasure_index treasures(_self, _self.value);
            auto iterator = treasures.find(treasurepkey);
            eosio_assert(iterator != treasures.end(), "Treasure not found.");
            eosio_assert(iterator->status == "active", "Treasure is not active.");
            
            //Owner of the treasure can only unlock a treasure if it's conquered by someone else. And if conquered, the
            //user that has conquered can not unlock as long as that account is registered as conquered.
            if(is_account( iterator->conqueredby))
                eosio_assert(iterator->conqueredby != from, "You are not allowed to unlock a treasure you have conquered.");
            else
                eosio_assert(iterator->owner != from, "You are not allowed to unlock your own treasure.");

            //Take percent of the transfered EOS as provision to the lost diamond owners
            eosio::asset totcrfund = (eos * (25 * 100)) / 10000;
            tcrfund_index tcrfund(_self, _self.value);
            //Update investors earnedpayout base on each investors percent
            for (auto itr = tcrfund.begin(); itr != tcrfund.end(); itr++) {
                eosio::asset earnedpayout = (totcrfund * (itr->investorpercent * 100)) / 10000;
                tcrfund.modify(itr, _self, [&]( auto& row ) {
                    row.earnedpayout = row.earnedpayout + earnedpayout;
                }); 
            }

            //Add 25 percent to the value of the Lost Diamond (account cptblackbill is used for this)
            auto existingiterator2 = tcrfund.find(to.value); //to = cptblackbill
            tcrfund.modify(existingiterator2, _self, [&]( auto& row ) {
                row.investedamount = row.investedamount + totcrfund;
            });
            
            //Add row to verifyunlock
            verifyunlock_index verifyunlock(_self, _self.value);
            verifyunlock.emplace(_self, [&]( auto& row ) {
                row.pkey = verifyunlock.available_primary_key();
                row.treasurepkey = treasurepkey;
                row.secretcode = secretcode;
                row.byaccount = from;
                row.addtochestamount = eos - (2 * totcrfund);
                row.timestamp = now();
            });
        }
        else if (memo.rfind("Activate SponsorItem No.", 0) == 0) {
            uint64_t sponsorItemPkey = std::strtoull( memo.substr(24).c_str(),NULL,0 ); //Find treasure pkey from transfer memo

            sponsoritems_index sponsoritems(_self, _self.value);
            auto iterator = sponsoritems.find(sponsorItemPkey);
            eosio_assert(iterator != sponsoritems.end(), "Sponsor item not found.");
            eosio_assert(iterator->status == "pendingforadfeepayment", "Sponsor item is not pending for payment.");
            eosio_assert(eos.amount >= iterator->adFeePrice.amount, "Payment amount is less than advertising fee.");

            //Take percent of the transfered EOS as provision to the lost diamond owners
            eosio::asset totcrfund = (eos * (10 * 100)) / 10000;
            tcrfund_index tcrfund(_self, _self.value);
            //Update investors earnedpayout base on each investors percent
            for (auto itr = tcrfund.begin(); itr != tcrfund.end(); itr++) {
                eosio::asset earnedpayout = (totcrfund * (itr->investorpercent * 100)) / 10000;
                tcrfund.modify(itr, _self, [&]( auto& row ) {
                    row.earnedpayout = row.earnedpayout + earnedpayout;
                }); 
            }

            //Add 10 percent to the value of the Lost Diamond (account cptblackbill is used for this)
            auto existingiterator2 = tcrfund.find(to.value); //to = cptblackbill
            tcrfund.modify(existingiterator2, _self, [&]( auto& row ) {
                row.investedamount = row.investedamount + totcrfund;
            });

            //The other 50% is added to the treasure value and provision to token holders

            sponsoritems.modify(iterator, _self, [&]( auto& row ) {
                row.status = "active";
            }); 
        }
        else{
            
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
            
            if(eos >= getPriceForCheckTreasureValueInEOS())
            {
                //Amounts are added to the lost diamond ownership as investment to provision of income 
                //from check- and unlock-transactions. Accounts that activate treasures are also added as lost
                //diamonds owners
                tcrfund_index tcrfund(_self, _self.value);

                //Insert or update amount to tcrf-fund for the from account.
                auto existingiterator = tcrfund.find(from.value);
                if(existingiterator == tcrfund.end()){
                    tcrfund.emplace(_self, [&]( auto& row ) {
                        row.account = from;
                        row.investedamount = eos;
                        row.investorpercent = 0; //Null by default. Will be recalculated later
                        row.earnedpayout = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                        row.timestamp = now();
                    });
                }
                else{
                    tcrfund.modify(existingiterator, _self, [&]( auto& row ) {
                        row.investedamount = row.investedamount + eos;
                    });            
                }
                
                //Get total amount invested from all accounts
                eosio::asset totalinvestedamount = eosio::asset(0, symbol(symbol_code("EOS"), 4));
                auto itr = tcrfund.upper_bound(0);
                for (auto itr = tcrfund.begin(); itr != tcrfund.end(); itr++) {
                    totalinvestedamount = totalinvestedamount + (*itr).investedamount;
                }

                //Update investor percent
                for (auto itr = tcrfund.begin(); itr != tcrfund.end(); itr++) {
                    double newinvestorpercent = 100;
                    if(totalinvestedamount.amount > 0)
                        newinvestorpercent = 100 * itr->investedamount.amount / totalinvestedamount.amount;  
                    
                    tcrfund.modify(itr, _self, [&]( auto& row ) {
                        row.investorpercent = newinvestorpercent;
                    }); 
                }

                cptblackbill::issue(from, eosio::asset(1000, symbol(symbol_code("BLKBILL"), 4)), std::string("Mined BLKBILLs for investing in the lost diamond.") );
            
            }
            else{
                //All other smaller amounts will initiate BLKBILL token issue
                cptblackbill::issue(from, eosio::asset(1, symbol(symbol_code("BLKBILL"), 4)), std::string("Mined BLKBILLS for using Captain Black Bill.") );
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
        
        treasures.modify(iterator, user, [&]( auto& row ) {
            row.title = title;
            row.description = description;
            row.imageurl = imageurl;
            row.videourl = videourl;
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

        treasures.modify(iterator, user, [&]( auto& row ) {
            row.latitude = latitude;
            row.longitude = longitude;
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
    void activatchest(uint64_t pkey) 
    {
        require_auth("cptblackbill"_n); //Only allowed by cptblackbill contract

        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(pkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        
        //Get number of unique accounts that has solved treasures created by treasure owner = rating points.
        uint64_t numberOfUniqueUserUnlocks = 0;
        std::set<uint64_t> uniqueUsersSet;

        results_index results(_code, _code.value);
        auto resultItems = results.get_index<"creator"_n>();
        auto iter = resultItems.find(iterator->owner.value);
        while (iter != resultItems.end()) {
            if(iter->creator != iterator->owner) //Stop when list is outside the values of the treasures creator
                break;
            
            uniqueUsersSet.insert(iter->user.value); //insert only add unique values to a set
            iter++;
        } 
        numberOfUniqueUserUnlocks = uniqueUsersSet.size();
        
        treasures.modify(iterator, _self, [&]( auto& row ) {
            row.status = "active";
            row.rankingpoint = numberOfUniqueUserUnlocks;
            row.expirationdate = now() + 94608000; //Treasure ownership renewed for three years
        });
    }

    [[eosio::action]]
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
    }

    [[eosio::action]]
    void unlockchest(uint64_t treasurepkey, asset payouteos, name byuser, bool lostdiamondisfound, 
                     uint64_t verifyunlockpkey, uint64_t sponsoritempkey) {
        require_auth("cptblackbill"_n); //Only allowed by cptblackbill contract

        //Get total amount in Lost Diamond if diamond is found in this treasure
        eosio::asset totalamountinlostdiamond = eosio::asset(0, symbol(symbol_code("EOS"), 4));
        if(lostdiamondisfound){
            tcrfund_index tcrfund(_self, _self.value);
            auto itr = tcrfund.upper_bound(0);
            for (auto itr = tcrfund.begin(); itr != tcrfund.end(); itr++) {
                totalamountinlostdiamond = totalamountinlostdiamond + (*itr).investedamount;
            }

            payouteos = payouteos + totalamountinlostdiamond; //Add lost diamond value to the treasure value
        }

        if(sponsoritempkey >= 0){
            sponsoritems_index sponsoritems(_self, _self.value);
            auto iterator = sponsoritems.find(sponsoritempkey);
            sponsoritems.modify(iterator, _self, [&]( auto& row ) {
                row.status = "robbed";
                row.wonby = byuser;
                row.treasurepkey = treasurepkey;
                row.wontimestamp = now();
            });  
        }
        
        treasure_index treasures(_code, _code.value);
        auto iterator = treasures.find(treasurepkey);
        eosio_assert(iterator != treasures.end(), "Treasure not found");
        name treasureowner = iterator->owner; 
        name treasureConquerer = iterator->conqueredby;

        treasures.modify(iterator, _self, [&]( auto& row ) {
            row.status = "robbed";
            
            if(byuser != treasureowner)
                row.conqueredby = byuser; //The treasure has been conquered by the robber. The robber has now access to activate the treasure with a new code.
            else
                row.conqueredby = ""_n; //Set as blank if owner has conquered back the treasure.
            
            if(payouteos.amount > 0) 
            {
                //Treasure has been unlocked by <byuser>. 
                //Split payout amount in two - since both creator and finder get an equal share of the treasure
                payouteos = payouteos / 2;

                //Transfer treasure chest value to the user who unlocked the treasure
                action(
                    permission_level{ get_self(), "active"_n },
                    "eosio.token"_n, "transfer"_n,
                    std::make_tuple(get_self(), byuser, payouteos, std::string("Congrats for solving Treasure No." + std::to_string(treasurepkey) + " on CptBlackBill!"))
                ).send();

                //Transfer the same amount to the user who created the treasure
                //Share the amount with the conquerer if the treasure has a conquerer
                if(byuser == treasureowner && is_account(treasureConquerer)){
                    //The treasure owner conquer back ownership to the treasure. Then send the other half to conquerer
                    action(
                        permission_level{ get_self(), "active"_n },
                        "eosio.token"_n, "transfer"_n,
                        std::make_tuple(get_self(), treasureConquerer, payouteos, std::string("Congrats! Treasure No." + std::to_string(treasurepkey) + " has been solved by the owner. This is your equal share of the treasure chest."))
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

                //Reward finder for using CptBlackBill
                cptblackbill::issue(byuser, eosio::asset(10000, symbol(symbol_code("BLKBILL"), 4)), std::string("1 BLKBILL token as reward for unlocking treasure.") );
                //send_summary(byuser, "1 BLKBILL token as reward for unlocking a treasure.");

                //Reward creator for creating content 
                cptblackbill::issue(treasureowner, eosio::asset(20000, symbol(symbol_code("BLKBILL"), 4)), std::string("2 BLKBILL token as reward for someone unlocking your treasure.") );
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
                    row.eosusdprice = getEosUsdPrice(); //2019-01-08
                    row.minedblkbills = eosio::asset(30000, symbol(symbol_code("BLKBILL"), 4));
                    row.timestamp = now();
                });

                //Remove verify unlock record
                verifyunlock_index verifyunlock(_code, _code.value);
                auto iterator = verifyunlock.find(verifyunlockpkey);
                eosio_assert(iterator != verifyunlock.end(), "Verify unlock record does not exist");
                verifyunlock.erase(iterator);

                if(lostdiamondisfound){
                    //Send earned income to the current lost diamond owners and delete rows
                    tcrfund_index tcrfund(_self, _self.value);
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
                    }
                } 
            }
        });
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
        eosio_assert(user == iterator->owner, "You don't have access to reset the secret code on this treasure.");
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
    void erasecheck(uint64_t pkey) {
        require_auth("cptblackbill"_n);
        
        verifycheck_index verifycheck(_code, _code.value);
        auto iterator = verifycheck.find(pkey);
        eosio_assert(iterator != verifycheck.end(), "Verify check value record does not exist");
        verifycheck.erase(iterator);
    }

    //This is used to add a verified unlock for sponsored treasures where no EOS payment is needed
    [[eosio::action]]
    void addverunlc(uint64_t treasurepkey, name byaccount, std::string secretcode) {
        require_auth("cptblackbill"_n);
        
        //Add row to verifyunlock
        verifyunlock_index verifyunlock(_self, _self.value);
        verifyunlock.emplace(_self, [&]( auto& row ) {
            row.pkey = verifyunlock.available_primary_key();
            row.treasurepkey = treasurepkey;
            row.secretcode = secretcode;
            row.byaccount = byaccount;
            row.addtochestamount = eosio::asset(0, symbol(symbol_code("EOS"), 4));
            row.timestamp = now();
        });
    }

    [[eosio::action]]
    void eraseverunlc(uint64_t pkey) {
        require_auth("cptblackbill"_n);
        
        verifyunlock_index verifyunlock(_code, _code.value);
        auto iterator = verifyunlock.find(pkey);
        eosio_assert(iterator != verifyunlock.end(), "Verify unlock record does not exist");
        verifyunlock.erase(iterator);
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
            row.treasurepkey = -1;
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
        uint64_t rankingpoint; //Calculated and updated by CptBlackBill based on video and turnover stats.  
        int32_t timestamp; //Date created
        int32_t expirationdate; //Date when ownership expires - other users can then take ownnership of this treasure location
        std::string secretcode;
        std::string status;
        eosio::name conqueredby; //If someone has robbed and conquered the treasure. Conquered by user will get 75% of the treasure value next time it's robbed. The owner will still get 25%
        std::string conqueredimg; //The user who conquered can add another image to the treasure.
        std::string jsondata;  //additional field for other info in json format.
        uint64_t primary_key() const { return  pkey; }
        uint64_t by_owner() const {return owner.value; } //second key, can be non-unique
        uint64_t by_rankingpoint() const {return rankingpoint; } //fourth key, can be non-unique
    };
    typedef eosio::multi_index<"treasure"_n, treasure, 
            eosio::indexed_by<"owner"_n, const_mem_fun<treasure, uint64_t, &treasure::by_owner>>,
            eosio::indexed_by<"rankingpoint"_n, const_mem_fun<treasure, uint64_t, &treasure::by_rankingpoint>>> treasure_index;

    struct [[eosio::table]] tcrfund {
        eosio::name account;
        eosio::asset investedamount;
        double investorpercent;
        eosio::asset earnedpayout;
        int32_t timestamp;

        uint64_t primary_key() const { return  account.value; }
    };
    typedef eosio::multi_index<"tcrfund"_n, tcrfund> tcrfund_index;
    
    struct [[eosio::table]] verifycheck {
        uint64_t pkey;
        uint64_t treasurepkey;
        eosio::name byaccount;
        eosio::asset addtochestamount;
        int32_t timestamp;

        uint64_t primary_key() const { return  pkey; }
    };
    typedef eosio::multi_index<"verifycheck"_n, verifycheck> verifycheck_index;

    struct [[eosio::table]] verifyunlock {
        uint64_t pkey;
        uint64_t treasurepkey;
        eosio::name byaccount;
        eosio::asset addtochestamount;
        std::string secretcode;
        int32_t timestamp;

        uint64_t primary_key() const { return  pkey; }
    };
    typedef eosio::multi_index<"verifyunlock"_n, verifyunlock> verifyunlock_index;

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
    if(code==receiver && action==name("addtreasure").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addtreasure );
    }
    else if(code==receiver && action==name("addtradmin").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addtradmin );
    }
    else if(code==receiver && action==name("modtreasure").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modtreasure );
    }
    else if(code==receiver && action==name("modtreasimg").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modtreasimg );
    }
    else if(code==receiver && action==name("modgps").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modgps );
    }
    else if(code==receiver && action==name("modtreasjson").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modtreasjson );
    }
    else if(code==receiver && action==name("activatchest").value) {
      execute_action(name(receiver), name(code), &cptblackbill::activatchest );
    }
    else if(code==receiver && action==name("updranking").value) {
      execute_action(name(receiver), name(code), &cptblackbill::updranking );
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
    else if(code==receiver && action==name("addsetting").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addsetting );
    }
    else if(code==receiver && action==name("modsetting").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modsetting );
    }
    else if(code==receiver && action==name("erasesetting").value) {
      execute_action(name(receiver), name(code), &cptblackbill::erasesetting );
    }
    else if(code==receiver && action==name("erasecheck").value) {
      execute_action(name(receiver), name(code), &cptblackbill::erasecheck );
    }
    else if(code==receiver && action==name("addverunlc").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addverunlc );
    }
    else if(code==receiver && action==name("eraseverunlc").value) {
      execute_action(name(receiver), name(code), &cptblackbill::eraseverunlc );
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

