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
            
            eosio::asset totcrfund = (eos * (25 * 100)) / 10000; //25 percent provision to diamond owners
            eosio::asset toLostDiamondValueByCptBlackBill = (eos * (20 * 100)) / 10000; //20 percent to diamond value 

            //Update diamond ownership for cptblackbill
            //The provision earned to account cptblackbill is transfered to a random treasure when the lost diamond is found
            diamondownrs_index diamondownrs(_self, _self.value);
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
            }

            //2020-02-24 Add to diamond fund
            eosio::asset toTokenHolders = (eos * (5 * 100)) / 10000;
            diamondfund_index diamondfund(_self, _self.value);
            auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
            auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
            diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                row.toDiamondOwners += totcrfund;
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

            if (from != name{"cptbbfinanc1"}) //2020-04-10 Normal payment for unlocking treasure (cptbbfinanc1 is used for free unlockings when user has no money in account)
            {
                eosio_assert(eos >= getPriceForCheckTreasureValueInEOS(), "Transfered amount is below minimum price for unlocking a treasure.");
                
                //Owner of the treasure can only unlock a treasure if it's conquered by someone else. And if conquered, the
                //user that has conquered can not unlock as long as that account is registered as conquered.
                if(is_account( iterator->conqueredby))
                    eosio_assert(iterator->conqueredby != from, "You are not allowed to unlock a treasure you have conquered.");
                else
                    eosio_assert(iterator->owner != from, "You are not allowed to unlock your own treasure.");

                //Take percent of the transfered EOS as provision to the lost diamond owners and diamond value
                eosio::asset totcrfund = (eos * (25 * 100)) / 10000;
                eosio::asset toLostDiamondValueByCptBlackBill = (eos * (20 * 100)) / 10000;

                //Add 20 percent to the value of the Lost Diamond (account cptblackbill is used for this)
                diamondownrs_index diamondownrs(_self, _self.value);
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
                }

                //2020-02-24 Add to diamond fund
                eosio::asset toTokenHolders = (eos * (5 * 100)) / 10000;
                diamondfund_index diamondfund(_self, _self.value);
                auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
                auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
                diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                    row.toDiamondOwners += totcrfund;
                    row.toTokenHolders += toTokenHolders;
                    row.diamondValue += toLostDiamondValueByCptBlackBill;
                });
            }
        }
        else if (memo.rfind("Activate SponsorItem No.", 0) == 0) {
            uint64_t sponsorItemPkey = std::strtoull( memo.substr(24).c_str(),NULL,0 ); //Find treasure pkey from transfer memo

            sponsoritems_index sponsoritems(_self, _self.value);
            auto iterator = sponsoritems.find(sponsorItemPkey);
            eosio_assert(iterator != sponsoritems.end(), "Sponsor item not found.");
            eosio_assert(iterator->status == "pendingforadfeepayment", "Sponsor item is not pending for payment.");
            eosio_assert(eos.amount >= iterator->adFeePrice.amount, "Payment amount is less than advertising fee.");

            ////Take percent of the transfered EOS as provision to the lost diamond owners
            eosio::asset totcrfund = (eos * (10 * 100)) / 10000;
            
            //2020-02-25: This will replace the code above
            //Add 10 percent to the value of the Lost Diamond (account cptblackbill is used for this)
            diamondownrs_index diamondownrs(_self, _self.value);
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
            }


            //2020-02-24 Add to diamond fund
            eosio::asset toTokenHolders = (eos * (30 * 100)) / 10000;
            diamondfund_index diamondfund(_self, _self.value);
            auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
            auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
            diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                row.toDiamondOwners += totcrfund; //10%
                row.toTokenHolders += toTokenHolders; //30%
                row.diamondValue += totcrfund; //10%
            });  

            //The other 50% is added to the treasure value 

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
                
                //Insert or update invested amount for diamond owner.
                diamondownrs_index diamondownrs(_self, _self.value);
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
                }
                
                //Add to diamond fund
                diamondfund_index diamondfund(_self, _self.value);
                auto diamondFundItr = diamondfund.rbegin(); //Find the last added diamond fund item
                auto diamondFundIterator = diamondfund.find(diamondFundItr->pkey);
                diamondfund.modify(diamondFundIterator, _self, [&]( auto& row ) {
                    row.diamondValue += eos; //100%
                });  

                cptblackbill::issue(from, eosio::asset(10, symbol(symbol_code("BLKBILL"), 4)), std::string("Mined BLKBILLs for investing in the lost diamond.") );
            
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

    [[eosio::action]]
    void activatchest(uint64_t pkey, std::string encryptedSecretCode) 
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
            row.secretcode = encryptedSecretCode;
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
    void unlockchest(uint64_t treasurepkey, asset payouteos, name byuser, bool lostdiamondisfound, uint64_t sponsoritempkey, bool isNoPaymentRobbery) {
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

        if(sponsoritempkey > 0){ //Sponsor item pKey must always be larger than 0. 
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
            //row.status = "robbed";
            
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

                if(isNoPaymentRobbery == true)
                    row.eosusdprice = eosio::asset(0, symbol(symbol_code("USD"), 4)); //2020-04-10 Zero to mark that this is a no payment robbery
                else
                    row.eosusdprice = getEosUsdPrice(); //2019-01-08
                
                row.minedblkbills = eosio::asset(10, symbol(symbol_code("BLKBILL"), 4));
                row.timestamp = now();
            });

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
    else if(code==receiver && action==name("btulla").value) {
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
    else if(code==receiver && action==name("addtradmin").value) {
      execute_action(name(receiver), name(code), &cptblackbill::addtradmin );
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
    else if(code==receiver && action==name("modtreasimg").value) {
      execute_action(name(receiver), name(code), &cptblackbill::modtreasimg );
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

