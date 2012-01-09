
#include <iostream>
#include <fstream>

#include <boost/lexical_cast.hpp>

#include "Application.h"
#include "Ledger.h"
#include "newcoin.pb.h"
#include "PackedMessage.h"
#include "Config.h"
#include "Conversion.h"
#include "BitcoinUtil.h"
#include "Wallet.h"

Ledger::Ledger(const uint160& masterID, uint64 startAmount) :
	mFeeHeld(0), mTimeStamp(0), mLedgerSeq(0),
	mClosed(false), mValidHash(false), mAccepted(false)
{
	mTransactionMap=SHAMap::pointer(new SHAMap());
	mAccountStateMap=SHAMap::pointer(new SHAMap());
	
	AccountState::pointer startAccount=AccountState::pointer(new AccountState(masterID));
	startAccount->credit(startAmount);
	if(!addAccountState(startAccount))
		assert(false);
}

Ledger::Ledger(const uint256 &parentHash, const uint256 &transHash, const uint256 &accountHash,
	uint64 feeHeld, uint64 timeStamp, uint32 ledgerSeq)
		: mParentHash(parentHash), mTransHash(transHash), mAccountHash(accountHash),
		mFeeHeld(feeHeld), mTimeStamp(timeStamp), mLedgerSeq(ledgerSeq),
		mClosed(false), mValidHash(false), mAccepted(false)
{
	updateHash();
}

Ledger::Ledger(Ledger &prevLedger, uint64 ts) : mTimeStamp(ts), 
	mClosed(false), mValidHash(false), mAccepted(false),
	mTransactionMap(new SHAMap()), mAccountStateMap(prevLedger.mAccountStateMap)
{
	mParentHash=prevLedger.getHash();
	mLedgerSeq=prevLedger.mLedgerSeq+1;
}

void Ledger::updateHash()
{
	Serializer s(116);
	addRaw(s);
	mHash=s.getSHA512Half();
	mValidHash=true;
}

void Ledger::addRaw(Serializer &s)
{
	s.add32(mLedgerSeq);
	s.add64(mFeeHeld);
	s.add256(mParentHash);
	s.add256(mTransHash);
	s.add256(mAccountHash);
	s.add64(mTimeStamp);
}

AccountState::pointer Ledger::getAccountState(const uint160& accountID)
{
#ifdef DEBUG
	std::cerr << "Ledger:getAccountState(" << accountID.GetHex() << ")" << std::endl;
#endif
	ScopedLock l(mTransactionMap->Lock());
	SHAMapItem::pointer item=mAccountStateMap->peekItem(accountID.to256());
	if(!item)
	{
#ifdef DEBUG
		std::cerr << "   notfound" << std::endl;
#endif
		return AccountState::pointer();
	}
	return AccountState::pointer(new AccountState(item->getData()));
}

uint64 Ledger::getBalance(const uint160& accountID)
{
	ScopedLock l(mTransactionMap->Lock());
	SHAMapItem::pointer item=mAccountStateMap->peekItem(accountID.to256());
	if(!item) return 0;
	return AccountState(item->getData()).getBalance();
}

bool Ledger::updateAccountState(AccountState::pointer state)
{
	assert(!mAccepted);
	SHAMapItem::pointer item(new SHAMapItem(state->getAccountID(), state->getRaw()));
	return mAccountStateMap->updateGiveItem(item);
}

bool Ledger::addAccountState(AccountState::pointer state)
{
	assert(!mAccepted);
	SHAMapItem::pointer item(new SHAMapItem(state->getAccountID(), state->getRaw()));
	return mAccountStateMap->addGiveItem(item);
}

bool Ledger::addTransaction(Transaction::pointer trans)
{ // low-level - just add to table
	assert(!mAccepted);
	assert(!!trans->getID());
	SHAMapItem::pointer item(new SHAMapItem(trans->getID(), trans->getSigned()->getData()));
	return mTransactionMap->addGiveItem(item);
}

bool Ledger::delTransaction(const uint256& transID)
{
	assert(!mAccepted);
	return mTransactionMap->delItem(transID); 
}

Transaction::pointer Ledger::getTransaction(const uint256& transID)
{
	ScopedLock l(mTransactionMap->Lock());
	SHAMapItem::pointer item=mTransactionMap->peekItem(transID);
	if(!item) return Transaction::pointer();
	Transaction::pointer trans(new Transaction(item->getData(), true));
	if(trans->getStatus()==NEW) trans->setStatus(mClosed ? COMMITTED : INCLUDED, mLedgerSeq);
	return trans;
}

Ledger::TransResult Ledger::applyTransaction(Transaction::pointer trans)
{
	assert(!mAccepted);
	boost::recursive_mutex::scoped_lock sl(mLock);
	if(trans->getSourceLedger()>mLedgerSeq) return TR_BADLSEQ;

	if(trans->getAmount()<trans->getFee())
	{
#ifdef DEBUG
			std::cerr << "Transaction for " << trans->getAmount() << ", but fee is " <<
					trans->getFee() << std::endl;
#endif
		return TR_TOOSMALL;
	}

	if(!mTransactionMap || !mAccountStateMap) return TR_ERROR;
	try
	{
		// already applied?
		Transaction::pointer dupTrans=getTransaction(trans->getID());
		if(dupTrans) return TR_ALREADY;

		// accounts exist?
		AccountState::pointer fromAccount=getAccountState(trans->getFromAccount());
		AccountState::pointer toAccount=getAccountState(trans->getToAccount());

		// temporary code -- if toAccount doesn't exist but fromAccount does, create it
		if(!!fromAccount && !toAccount)
		{
			toAccount=AccountState::pointer(new AccountState(trans->getToAccount()));
			toAccount->incSeq(); // an account in a ledger has a sequence of 1
			updateAccountState(toAccount);
		}

		if(!fromAccount || !toAccount) return TR_BADACCT;

		// pass sanity checks?
		if(fromAccount->getBalance()<trans->getAmount())
		{
#ifdef DEBUG
			std::cerr << "Transaction for " << trans->getAmount() << ", but account has " <<
					fromAccount->getBalance() << std::endl;
#endif
			return TR_INSUFF;
		}
#ifdef DEBUG
		if(fromAccount->getSeq()!=trans->getFromAccountSeq())
			std::cerr << "aSeq=" << fromAccount->getSeq() << ", tSeq=" << trans->getFromAccountSeq() << std::endl;
#endif
		if(fromAccount->getSeq()>trans->getFromAccountSeq()) return TR_PASTASEQ;
		if(fromAccount->getSeq()<trans->getFromAccountSeq()) return TR_PREASEQ;

		// apply
		fromAccount->charge(trans->getAmount());
		fromAccount->incSeq();
		toAccount->credit(trans->getAmount()-trans->getFee());
		mFeeHeld+=trans->getFee();
		trans->setStatus(INCLUDED, mLedgerSeq);

		updateAccountState(fromAccount);
		updateAccountState(toAccount);
		addTransaction(trans);

		return TR_SUCCESS;
	}
	catch (SHAMapException)
	{
		return TR_ERROR;
	}
}

Ledger::TransResult Ledger::removeTransaction(Transaction::pointer trans)
{ // high-level - reverse application of transaction
	assert(!mAccepted);
	boost::recursive_mutex::scoped_lock sl(mLock);
	if(!mTransactionMap || !mAccountStateMap) return TR_ERROR;
	try
	{
		Transaction::pointer ourTrans=getTransaction(trans->getID());
		if(!ourTrans) return TR_NOTFOUND;

		// accounts exist
		AccountState::pointer fromAccount=getAccountState(trans->getFromAccount());
		AccountState::pointer toAccount=getAccountState(trans->getToAccount());
		if(!fromAccount || !toAccount) return TR_BADACCT;

		// pass sanity checks?
		if(toAccount->getBalance()<trans->getAmount()) return TR_INSUFF;
		if(fromAccount->getSeq()!=(trans->getFromAccountSeq()+1)) return TR_PASTASEQ;
		
		// reverse
		fromAccount->credit(trans->getAmount());
		fromAccount->decSeq();
		toAccount->charge(trans->getAmount()-trans->getFee());
		mFeeHeld-=trans->getFee();
		trans->setStatus(REMOVED, mLedgerSeq);
		
		if(!delTransaction(trans->getID()))
		{
			assert(false);
			return TR_ERROR;
		}
		updateAccountState(fromAccount);
		updateAccountState(toAccount);
		return TR_SUCCESS;
	}
	catch (SHAMapException)
	{
		return TR_ERROR;
	}
}

Ledger::TransResult Ledger::hasTransaction(Transaction::pointer trans)
{
	boost::recursive_mutex::scoped_lock sl(mLock);
	if(mTransactionMap==NULL) return TR_ERROR;
	try
	{
		Transaction::pointer t=getTransaction(trans->getID());
		if(t==NULL) return TR_NOTFOUND;
		return TR_SUCCESS;
	}
	catch (SHAMapException)
	{
		return TR_ERROR;
	}
}

Ledger::pointer Ledger::closeLedger(uint64 timeStamp)
{ // close this ledger, return a pointer to the next ledger
  // CAUTION: New ledger needs its SHAMap's connected to storage
	setClosed();
	return Ledger::pointer(new Ledger(*this, timeStamp));
}

bool Ledger::unitTest()
{
	uint160 la1=theApp->getWallet().addFamily(CKey::PassPhraseToKey("This is my payphrase!"), false);
	uint160 la2=theApp->getWallet().addFamily(CKey::PassPhraseToKey("Another payphrase"), false);

	LocalAccount::pointer l1=theApp->getWallet().getLocalAccount(la1, 0);
	LocalAccount::pointer l2=theApp->getWallet().getLocalAccount(la2, 0);

	assert(l1->getAddress()==la1);

#ifdef DEBUG
	std::cerr << "Account1: " << la1.GetHex() << std::endl;
	std::cerr << "Account2: " << la2.GetHex() << std::endl;
#endif

	Ledger::pointer ledger(new Ledger(la1, 100000));
	
	ledger=Ledger::pointer(new Ledger(*ledger, 0));

	AccountState::pointer as=ledger->getAccountState(la1);
	assert(as);
	assert(as->getBalance()==100000);
	assert(as->getSeq()==0);
	as=ledger->getAccountState(la2);
	assert(!as); 

	Transaction::pointer t(new Transaction(NEW, l1, l1->getAcctSeq(), l2->getAddress(), 2500, 0, 1));
	assert(!!t->getID());

	Ledger::TransResult tr=ledger->applyTransaction(t);
#ifdef DEBUG
	std::cerr << "Transaction: " << tr << std::endl;
#endif
	assert(tr==TR_SUCCESS);

	return true;
}

uint256 Ledger::getHash()
{
	if(!mValidHash) updateHash();
	return(mHash); 
}

void Ledger::saveAcceptedLedger(Ledger::pointer ledger)
{
	std::string sql="INSERT INTO Ledgers "
		"(LedgerHash,LedgerSeq,PrevHash,FeeHeld,ClosingTime,AccountSetHash,TransSetHash) VALUES ('";
	sql.append(ledger->getHash().GetHex());
	sql.append("','");
	sql.append(boost::lexical_cast<std::string>(ledger->mLedgerSeq));
	sql.append("','");
	sql.append(ledger->mParentHash.GetHex());
	sql.append("','");
	sql.append(boost::lexical_cast<std::string>(ledger->mFeeHeld));
	sql.append("','");
	sql.append(boost::lexical_cast<std::string>(ledger->mTimeStamp));
	sql.append("','");
	sql.append(ledger->mAccountHash.GetHex());
	sql.append("','");
	sql.append(ledger->mTransHash.GetHex());
	sql.append("');");

	ScopedLock sl(theApp->getLedgerDB()->getDBLock());
	theApp->getLedgerDB()->getDB()->executeSQL(sql.c_str());

	// write out dirty nodes
	while(ledger->mTransactionMap->flushDirty(64, TRANSACTION_NODE, ledger->mLedgerSeq))
	{ ; }
	while(ledger->mAccountStateMap->flushDirty(64, ACCOUNT_NODE, ledger->mLedgerSeq))
	{ ; }

}

Ledger::pointer Ledger::getSQL(const std::string& sql)
{
	uint256 ledgerHash, prevHash, accountHash, transHash;
	uint64 feeHeld, closingTime;
	uint32 ledgerSeq;
	std::string hash;

	if(1)
	{
		ScopedLock sl(theApp->getLedgerDB()->getDBLock());
		Database *db=theApp->getLedgerDB()->getDB();
		if(!db->executeSQL(sql.c_str()) || !db->startIterRows() || !db->getNextRow())
			 return Ledger::pointer();

		db->getStr("LedgerHash", hash);
		ledgerHash.SetHex(hash);
		db->getStr("PrevHash", hash);
		prevHash.SetHex(hash);
		db->getStr("AccountSetHash", hash);
		accountHash.SetHex(hash);
		db->getStr("TransSetHash", hash);
		transHash.SetHex(hash);
		feeHeld=db->getBigInt("FeeHeld");
		closingTime=db->getBigInt("ClosingTime");
		ledgerSeq=db->getBigInt("LedgerSeq");
		db->endIterRows();
	}
	
	Ledger::pointer ret(new Ledger(prevHash, transHash, accountHash, feeHeld, closingTime, ledgerSeq));
	if(ret->getHash()!=ledgerHash)
	{
		assert(false);
		return Ledger::pointer();
	}
	return ret;
}

Ledger::pointer Ledger::loadByIndex(uint32 ledgerIndex)
{
	std::string sql="SELECT * from Ledgers WHERE LedgerSeq='";
	sql.append(boost::lexical_cast<std::string>(ledgerIndex));
	sql.append("';");
	return getSQL(sql);
}

Ledger::pointer Ledger::loadByHash(const uint256& ledgerHash)
{
	std::string sql="SELECT * from Ledgers WHERE LedgerHash='";
	sql.append(ledgerHash.GetHex());
	sql.append("';");
	return getSQL(sql);
}

#if 0
// returns true if the from account has enough for the transaction and seq num is correct
bool Ledger::addTransaction(Transaction::pointer trans,bool checkDuplicate)
{
	if(checkDuplicate && hasTransaction(trans)) return(false);

	if(mParent)
	{ // check the lineage of the from addresses
		uint160 address=protobufTo160(trans->from());
		if(mAccounts.count(address))
		{
			pair<uint64,uint32> account=mAccounts[address];
			if( (account.first<trans->amount()) &&
				(trans->seqnum()==account.second) )
			{
				account.first -= trans->amount();
				account.second++;
				mAccounts[address]=account;


				uint160 destAddress=protobufTo160(trans->dest());

				Account destAccount=mAccounts[destAddress];
				destAccount.first += trans->amount();
				mAccounts[destAddress]=destAccount;


				mValidSig=false;
				mValidHash=false;
				mTransactions.push_back(trans);
				if(mChild)
				{
					mChild->parentAddedTransaction(trans);
				}
				return(true);
			}else
			{
				mDiscardedTransactions.push_back(trans);
				return false;
			}
		}else 
		{
			mDiscardedTransactions.push_back(trans);
			return false;
		}
		
	}else
	{ // we have no way to know so just hold on to it but don't add to the accounts
		mValidSig=false;
		mValidHash=false;
		mDiscardedTransactions.push_back(trans);
		return(true);
	}
}

// Don't check the amounts. We will do this at the end.
void Ledger::addTransactionAllowNeg(Transaction::pointer trans)
{
	uint160 fromAddress=protobufTo160(trans->from());

	if(mAccounts.count(fromAddress))
	{
		Account fromAccount=mAccounts[fromAddress];
		if(trans->seqnum()==fromAccount.second) 
		{
			fromAccount.first -= trans->amount();
			fromAccount.second++;
			mAccounts[fromAddress]=fromAccount;
			
			uint160 destAddress=protobufTo160(trans->dest());

			Account destAccount=mAccounts[destAddress];
			destAccount.first += trans->amount();
			mAccounts[destAddress]=destAccount;

			mTransactions.push_back(trans);
	
		}else
		{  // invalid seqnum
			mDiscardedTransactions.push_back(trans);
		}
	}else
	{
		if(trans->seqnum()==0)
		{
			
			mAccounts[fromAddress]=Account(-((int64)trans->amount()),1);

			uint160 destAddress=protobufTo160(trans->dest());

			Account destAccount=mAccounts[destAddress];
			destAccount.first += trans->amount();
			mAccounts[destAddress]=destAccount;

			mTransactions.push_back(trans);

		}else
		{
			mDiscardedTransactions.push_back(trans);
		}
	}
}



// Must look for transactions to discard to make this account positive
// When we chuck transactions it might cause other accounts to need correcting
void Ledger::correctAccount(const uint160& address)
{
	list<uint160> effected;

	// do this in reverse so we take of the higher seqnum first
	for( list<Transaction::pointer>::reverse_iterator iter=mTransactions.rbegin(); iter != mTransactions.rend(); )
	{
		Transaction::pointer trans= *iter;
		if(protobufTo160(trans->from()) == address)
		{
			Account fromAccount=mAccounts[address];
			assert(fromAccount.second==trans->seqnum()+1);
			if(fromAccount.first<0)
			{
				fromAccount.first += trans->amount();
				fromAccount.second --;

				mAccounts[address]=fromAccount;

				uint160 destAddress=protobufTo160(trans->dest());
				Account destAccount=mAccounts[destAddress];
				destAccount.first -= trans->amount();
				mAccounts[destAddress]=destAccount;
				if(destAccount.first<0) effected.push_back(destAddress);

				list<Transaction::pointer>::iterator temp=mTransactions.erase( --iter.base() );
				if(fromAccount.first>=0) break; 

				iter=list<Transaction::pointer>::reverse_iterator(temp);
			}else break;	
		}else iter--;
	}

	BOOST_FOREACH(uint160& address,effected)
	{
		correctAccount(address);
	}

}

#endif
