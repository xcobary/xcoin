// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "alert.h"
#include "checkpoints.h"
#include "db.h"
#include "net.h"
#include "init.h"
#include "ui_interface.h"
#include <boost/algorithm/string/replace.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>

using namespace std;
using namespace boost;

//
// Global state
//

CCriticalSection cs_setpwalletRegistered;
set<CWallet*> setpwalletRegistered;

CCriticalSection cs_main;

CTxMemPool mempool;
unsigned int nTransactionsUpdated = 0;

map<uint256, CBlockIndex*> mapBlockIndex;
uint256 hashGenesisBlock("0x000000005b1e3d23ecfd2dd4a6e1a35238aa0392c0a8528c40df52376d7efe2c");
static CBigNum bnProofOfWorkLimit(~uint256(0) >> 32);
CBlockIndex* pindexGenesisBlock = NULL;
int nBestHeight = -1;
CBigNum bnBestChainWork = 0;
CBigNum bnBestInvalidWork = 0;
uint256 hashBestChain = 0;
CBlockIndex* pindexBest = NULL;
int64 nTimeBestReceived = 0;

CMedianFilter<int> cPeerBlockCounts(5, 0); // Amount of blocks that other nodes claim to have

map<uint256, CBlock*> mapOrphanBlocks;
multimap<uint256, CBlock*> mapOrphanBlocksByPrev;

map<uint256, CDataStream*> mapOrphanTransactions;
map<uint256, map<uint256, CDataStream*> > mapOrphanTransactionsByPrev;

// Constant stuff for coinbase transactions we create:
CScript COINBASE_FLAGS;

const string strMessageMagic = "Xcoin Signed Message:\n";

double dHashesPerSec;
int64 nHPSTimerStart;

// Settings
mpq nTransactionFee = 0;



//////////////////////////////////////////////////////////////////////////////
//
// dispatching functions
//

// These functions dispatch to one or all registered wallets


void RegisterWallet(CWallet* pwalletIn)
{
    {
        LOCK(cs_setpwalletRegistered);
        setpwalletRegistered.insert(pwalletIn);
    }
}

void UnregisterWallet(CWallet* pwalletIn)
{
    {
        LOCK(cs_setpwalletRegistered);
        setpwalletRegistered.erase(pwalletIn);
    }
}

// check whether the passed transaction is from us
bool static IsFromMe(CTransaction& tx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        if (pwallet->IsFromMe(tx))
            return true;
    return false;
}

// get the wallet transaction with the given hash (if it exists)
bool static GetTransaction(const uint256& hashTx, CWalletTx& wtx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        if (pwallet->GetTransaction(hashTx,wtx))
            return true;
    return false;
}

// erases transaction with the given hash from all wallets
void static EraseFromWallets(uint256 hash)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->EraseFromWallet(hash);
}

// make sure all wallets know about the given transaction, in the given block
void SyncWithWallets(const CTransaction& tx, const CBlock* pblock, bool fUpdate)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->AddToWalletIfInvolvingMe(tx, pblock, fUpdate);
}

// notify wallets about a new best chain
void static SetBestChain(const CBlockLocator& loc)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->SetBestChain(loc);
}

// notify wallets about an updated transaction
void static UpdatedTransaction(const uint256& hashTx)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->UpdatedTransaction(hashTx);
}

// dump all wallets
void static PrintWallets(const CBlock& block)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->PrintWallet(block);
}

// notify wallets about an incoming inventory (for request counts)
void static Inventory(const uint256& hash)
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->Inventory(hash);
}

// ask wallets to resend their transactions
void static ResendWalletTransactions()
{
    BOOST_FOREACH(CWallet* pwallet, setpwalletRegistered)
        pwallet->ResendWalletTransactions();
}







//////////////////////////////////////////////////////////////////////////////
//
// mapOrphanTransactions
//

bool AddOrphanTx(const CDataStream& vMsg)
{
    CTransaction tx;
    CDataStream(vMsg) >> tx;
    uint256 hash = tx.GetHash();
    if (mapOrphanTransactions.count(hash))
        return false;

    CDataStream* pvMsg = new CDataStream(vMsg);

    // Ignore big transactions, to avoid a
    // send-big-orphans memory exhaustion attack. If a peer has a legitimate
    // large transaction with a missing parent then we assume
    // it will rebroadcast it later, after the parent transaction(s)
    // have been mined or received.
    // 10,000 orphans, each of which is at most 5,000 bytes big is
    // at most 500 megabytes of orphans:
    if (pvMsg->size() > 5000)
    {
        printf("ignoring large orphan tx (size: %"PRIszu", hash: %s)\n", pvMsg->size(), hash.ToString().substr(0,10).c_str());
        delete pvMsg;
        return false;
    }

    mapOrphanTransactions[hash] = pvMsg;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
        mapOrphanTransactionsByPrev[txin.prevout.hash].insert(make_pair(hash, pvMsg));

    printf("stored orphan tx %s (mapsz %"PRIszu")\n", hash.ToString().substr(0,10).c_str(),
        mapOrphanTransactions.size());
    return true;
}

void static EraseOrphanTx(uint256 hash)
{
    if (!mapOrphanTransactions.count(hash))
        return;
    const CDataStream* pvMsg = mapOrphanTransactions[hash];
    CTransaction tx;
    CDataStream(*pvMsg) >> tx;
    BOOST_FOREACH(const CTxIn& txin, tx.vin)
    {
        mapOrphanTransactionsByPrev[txin.prevout.hash].erase(hash);
        if (mapOrphanTransactionsByPrev[txin.prevout.hash].empty())
            mapOrphanTransactionsByPrev.erase(txin.prevout.hash);
    }
    delete pvMsg;
    mapOrphanTransactions.erase(hash);
}

unsigned int LimitOrphanTxSize(unsigned int nMaxOrphans)
{
    unsigned int nEvicted = 0;
    while (mapOrphanTransactions.size() > nMaxOrphans)
    {
        // Evict a random orphan:
        uint256 randomhash = GetRandHash();
        map<uint256, CDataStream*>::iterator it = mapOrphanTransactions.lower_bound(randomhash);
        if (it == mapOrphanTransactions.end())
            it = mapOrphanTransactions.begin();
        EraseOrphanTx(it->first);
        ++nEvicted;
    }
    return nEvicted;
}







//////////////////////////////////////////////////////////////////////////////
//
// CTransaction and CTxIndex
//

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout, CTxIndex& txindexRet)
{
    SetNull();
    if (!txdb.ReadTxIndex(prevout.hash, txindexRet))
        return false;
    if (!ReadFromDisk(txindexRet.pos))
        return false;
    if (prevout.n >= vout.size())
    {
        SetNull();
        return false;
    }
    return true;
}

bool CTransaction::ReadFromDisk(CTxDB& txdb, COutPoint prevout)
{
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

bool CTransaction::ReadFromDisk(COutPoint prevout)
{
    CTxDB txdb("r");
    CTxIndex txindex;
    return ReadFromDisk(txdb, prevout, txindex);
}

bool CTransaction::IsStandard() const
{
    if (nVersion > CTransaction::CURRENT_VERSION)
        return false;

    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        // Biggest 'standard' txin is a 3-signature 3-of-3 CHECKMULTISIG
        // pay-to-script-hash, which is 3 ~80-byte signatures, 3
        // ~65-byte public keys, plus a few script ops.
        if (txin.scriptSig.size() > 500)
            return false;
        if (!txin.scriptSig.IsPushOnly())
            return false;
    }
    BOOST_FOREACH(const CTxOut& txout, vout) {
        if (!::IsStandard(txout.scriptPubKey))
            return false;
        if (txout.nValue == 0)
            return false;
    }
    return true;
}

//
// Check transaction inputs, and make sure any
// pay-to-script-hash transactions are evaluating IsStandard scripts
//
// Why bother? To avoid denial-of-service attacks; an attacker
// can submit a standard HASH... OP_EQUAL transaction,
// which will get accepted into blocks. The redemption
// script can be anything; an attacker could use a very
// expensive-to-check-upon-redemption script like:
//   DUP CHECKSIG DROP ... repeated 100 times... OP_1
//
bool CTransaction::AreInputsStandard(const MapPrevTx& mapInputs) const
{
    if (IsCoinBase())
        return true; // Coinbases don't use vin normally

    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const CTxOut& prev = GetOutputFor(vin[i], mapInputs);

        vector<vector<unsigned char> > vSolutions;
        txnouttype whichType;
        // get the scriptPubKey corresponding to this input:
        const CScript& prevScript = prev.scriptPubKey;
        if (!Solver(prevScript, whichType, vSolutions))
            return false;
        int nArgsExpected = ScriptSigArgsExpected(whichType, vSolutions);
        if (nArgsExpected < 0)
            return false;

        // Transactions with extra stuff in their scriptSigs are
        // non-standard. Note that this EvalScript() call will
        // be quick, because if there are any operations
        // beside "push data" in the scriptSig the
        // IsStandard() call returns false
        vector<vector<unsigned char> > stack;
        if (!EvalScript(stack, vin[i].scriptSig, *this, i, 0))
            return false;

        if (whichType == TX_SCRIPTHASH)
        {
            if (stack.empty())
                return false;
            CScript subscript(stack.back().begin(), stack.back().end());
            vector<vector<unsigned char> > vSolutions2;
            txnouttype whichType2;
            if (!Solver(subscript, whichType2, vSolutions2))
                return false;
            if (whichType2 == TX_SCRIPTHASH)
                return false;

            int tmpExpected;
            tmpExpected = ScriptSigArgsExpected(whichType2, vSolutions2);
            if (tmpExpected < 0)
                return false;
            nArgsExpected += tmpExpected;
        }

        if (stack.size() != (unsigned int)nArgsExpected)
            return false;
    }

    return true;
}

unsigned int
CTransaction::GetLegacySigOpCount() const
{
    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        nSigOps += txin.scriptSig.GetSigOpCount(false);
    }
    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        nSigOps += txout.scriptPubKey.GetSigOpCount(false);
    }
    return nSigOps;
}


int CMerkleTx::SetMerkleBranch(const CBlock* pblock)
{
    if (fClient)
    {
        if (hashBlock == 0)
            return 0;
    }
    else
    {
        CBlock blockTmp;
        if (pblock == NULL)
        {
            // Load the block this tx is in
            CTxIndex txindex;
            if (!CTxDB("r").ReadTxIndex(GetHash(), txindex))
                return 0;
            if (!blockTmp.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos))
                return 0;
            pblock = &blockTmp;
        }

        // Update the tx's hashBlock
        hashBlock = pblock->GetHash();

        // Locate the transaction
        for (nIndex = 0; nIndex < (int)pblock->vtx.size(); nIndex++)
            if (pblock->vtx[nIndex] == *(CTransaction*)this)
                break;
        if (nIndex == (int)pblock->vtx.size())
        {
            vMerkleBranch.clear();
            nIndex = -1;
            printf("ERROR: SetMerkleBranch() : couldn't find tx in block\n");
            return 0;
        }

        // Fill in merkle branch
        vMerkleBranch = pblock->GetMerkleBranch(nIndex);
    }

    // Is the tx in a block that's in the main chain
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    return pindexBest->nHeight - pindex->nHeight + 1;
}







bool CTransaction::CheckTransaction() const
{
    // Basic checks that don't depend on any context
    if (vin.empty())
        return DoS(10, error("CTransaction::CheckTransaction() : vin empty"));
    if (vout.empty())
        return DoS(10, error("CTransaction::CheckTransaction() : vout empty"));
    // Size limits
    if (::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return DoS(100, error("CTransaction::CheckTransaction() : size limits failed"));
    if (nRefHeight < 0)
        return DoS(100, error("CTransaction::CheckTransaction() : nRefHeight less than zero"));

    // Check for negative or overflow output values
    int64 nValueOut = 0;
    BOOST_FOREACH(const CTxOut& txout, vout)
    {
        if (txout.nValue < 0)
            return DoS(100, error("CTransaction::CheckTransaction() : txout.nValue negative"));
        if (txout.nValue > I64_MAX_MONEY)
            return DoS(100, error("CTransaction::CheckTransaction() : txout.nValue too high"));
        nValueOut += txout.nValue;
        if (!MoneyRange(nValueOut))
            return DoS(100, error("CTransaction::CheckTransaction() : txout total out of range"));
    }

    // Check for duplicate inputs
    set<COutPoint> vInOutPoints;
    BOOST_FOREACH(const CTxIn& txin, vin)
    {
        if (vInOutPoints.count(txin.prevout))
            return false;
        vInOutPoints.insert(txin.prevout);
    }

    if (IsCoinBase())
    {
        if (vin[0].scriptSig.size() < 2 || vin[0].scriptSig.size() > 100)
            return DoS(100, error("CTransaction::CheckTransaction() : coinbase script size"));
    }
    else
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
            if (txin.prevout.IsNull())
                return DoS(10, error("CTransaction::CheckTransaction() : prevout is null"));
    }

    return true;
}

mpq CTransaction::GetMinFee(unsigned int nBlockSize, bool fAllowFree,
                            enum GetMinFee_mode mode) const
{
    // Base fee is either MIN_TX_FEE or MIN_RELAY_TX_FEE
    mpq nBaseFee = (mode == GMF_RELAY) ? MIN_RELAY_TX_FEE : MIN_TX_FEE;

    unsigned int nBytes = ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION);
    unsigned int nNewBlockSize = nBlockSize + nBytes;
    mpq nMinFee = (1 + nBytes / 1000) * nBaseFee;

    if (fAllowFree)
    {
        if (nBlockSize == 1)
        {
            // Transactions under 10K are free
            // (about 4500 BTC if made of 50 BTC inputs)
            if (nBytes < 10000)
                nMinFee = 0;
        }
        else
        {
            // Free transaction area
            if (nNewBlockSize < 27000)
                nMinFee = 0;
        }
    }

    // To limit dust spam, require MIN_TX_FEE/MIN_RELAY_TX_FEE if any output is less than 0.01
    if (nMinFee < nBaseFee)
    {
        BOOST_FOREACH(const CTxOut& txout, vout)
            if (i64_to_mpq(txout.nValue) < CENT)
                nMinFee = nBaseFee;
    }

    // Raise the price as the block approaches full
    if (nBlockSize != 1 && nNewBlockSize >= MAX_BLOCK_SIZE_GEN/2)
    {
        if (nNewBlockSize >= MAX_BLOCK_SIZE_GEN)
            return MPQ_MAX_MONEY;
        nMinFee *= MAX_BLOCK_SIZE_GEN / (MAX_BLOCK_SIZE_GEN - nNewBlockSize);
    }

    if (!MoneyRange(nMinFee))
        nMinFee = MPQ_MAX_MONEY;
    return nMinFee;
}


bool CTxMemPool::accept(CTxDB& txdb, CTransaction &tx, bool fCheckInputs,
                        bool* pfMissingInputs)
{
    if (pfMissingInputs)
        *pfMissingInputs = false;

    if (!tx.CheckTransaction())
        return error("CTxMemPool::accept() : CheckTransaction failed");

    if ( tx.nRefHeight > (nBestHeight + 20) )
        return error("CTxMemPool::accept() : tx.nRefHeight too high");

    // Coinbase is only valid in a block, not as a loose transaction
    if (tx.IsCoinBase())
        return tx.DoS(100, error("CTxMemPool::accept() : coinbase as individual tx"));

    // To help v0.1.5 clients who would see it as a negative number
    if ((int64)tx.nLockTime > std::numeric_limits<int>::max())
        return error("CTxMemPool::accept() : not accepting nLockTime beyond 2038 yet");

    // Rather not work on nonstandard transactions (unless -testnet)
    if (!fTestNet && !tx.IsStandard())
        return error("CTxMemPool::accept() : nonstandard transaction type");

    // Do we already have it?
    uint256 hash = tx.GetHash();
    {
        LOCK(cs);
        if (mapTx.count(hash))
            return false;
    }
    if (fCheckInputs)
        if (txdb.ContainsTx(hash))
            return false;

    // Check for conflicts with in-memory transactions
    CTransaction* ptxOld = NULL;
    for (unsigned int i = 0; i < tx.vin.size(); i++)
    {
        COutPoint outpoint = tx.vin[i].prevout;
        if (mapNextTx.count(outpoint))
        {
            // Disable replacement feature for now
            return false;

            // Allow replacing with a newer version of the same transaction
            if (i != 0)
                return false;
            ptxOld = mapNextTx[outpoint].ptx;
            if (ptxOld->IsFinal())
                return false;
            if (!tx.IsNewerThan(*ptxOld))
                return false;
            for (unsigned int i = 0; i < tx.vin.size(); i++)
            {
                COutPoint outpoint = tx.vin[i].prevout;
                if (!mapNextTx.count(outpoint) || mapNextTx[outpoint].ptx != ptxOld)
                    return false;
            }
            break;
        }
    }

    if (fCheckInputs)
    {
        MapPrevTx mapInputs;
        map<uint256, CTxIndex> mapUnused;
        bool fInvalid = false;
        if (!tx.FetchInputs(txdb, mapUnused, false, false, mapInputs, fInvalid))
        {
            if (fInvalid)
                return error("CTxMemPool::accept() : FetchInputs found invalid tx %s", hash.ToString().substr(0,10).c_str());
            if (pfMissingInputs)
                *pfMissingInputs = true;
            return false;
        }

        // Check for non-standard pay-to-script-hash in inputs
        if (!tx.AreInputsStandard(mapInputs) && !fTestNet)
            return error("CTxMemPool::accept() : nonstandard transaction input");

        // Note: if you modify this code to accept non-standard transactions, then
        // you should add code here to check that the transaction does a
        // reasonable number of ECDSA signature verifications.

        mpq nFees = tx.GetValueIn(mapInputs) - tx.GetValueOut();
        unsigned int nSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);

        // Don't accept it if it can't get into a block
        mpq txMinFee = tx.GetMinFee(1000, true, GMF_RELAY);
        if (nFees < txMinFee)
            return error("CTxMemPool::accept() : not enough fees %s, %s < %s",
                         hash.ToString().c_str(),
                         FormatMoney(nFees).c_str(),
                         FormatMoney(txMinFee).c_str());

        // Continuously rate-limit free transactions
        // This mitigates 'penny-flooding' -- sending thousands of free transactions just to
        // be annoying or make others' transactions take longer to confirm.
        if (nFees < MIN_RELAY_TX_FEE)
        {
            static CCriticalSection cs;
            static double dFreeCount;
            static int64 nLastTime;
            int64 nNow = GetTime();

            {
                LOCK(cs);
                // Use an exponentially decaying ~10-minute window:
                dFreeCount *= pow(1.0 - 1.0/600.0, (double)(nNow - nLastTime));
                nLastTime = nNow;
                // -limitfreerelay unit is thousand-bytes-per-minute
                // At default rate it would take over a month to fill 1GB
                if (dFreeCount > GetArg("-limitfreerelay", 15)*10*1000 && !IsFromMe(tx))
                    return error("CTxMemPool::accept() : free transaction rejected by rate limiter");
                if (fDebug)
                    printf("Rate limit dFreeCount: %g => %g\n", dFreeCount, dFreeCount+nSize);
                dFreeCount += nSize;
            }
        }

        // Check against previous transactions
        // This is done last to help prevent CPU exhaustion denial-of-service attacks.
        if (!tx.ConnectInputs(mapInputs, mapUnused, CDiskTxPos(1,1,1), pindexBest, false, false))
        {
            return error("CTxMemPool::accept() : ConnectInputs failed %s", hash.ToString().substr(0,10).c_str());
        }
    }

    // Store transaction in memory
    {
        LOCK(cs);
        if (ptxOld)
        {
            printf("CTxMemPool::accept() : replacing tx %s with new version\n", ptxOld->GetHash().ToString().c_str());
            remove(*ptxOld);
        }
        addUnchecked(hash, tx);
    }

    ///// are we sure this is ok when loading transactions or restoring block txes
    // If updated, erase old tx from wallet
    if (ptxOld)
        EraseFromWallets(ptxOld->GetHash());

    printf("CTxMemPool::accept() : accepted %s (poolsz %"PRIszu")\n",
           hash.ToString().substr(0,10).c_str(),
           mapTx.size());
    return true;
}

bool CTransaction::AcceptToMemoryPool(CTxDB& txdb, bool fCheckInputs, bool* pfMissingInputs)
{
    return mempool.accept(txdb, *this, fCheckInputs, pfMissingInputs);
}

bool CTxMemPool::addUnchecked(const uint256& hash, CTransaction &tx)
{
    // Add to memory pool without checking anything.  Don't call this directly,
    // call CTxMemPool::accept to properly check the transaction first.
    {
        mapTx[hash] = tx;
        for (unsigned int i = 0; i < tx.vin.size(); i++)
            mapNextTx[tx.vin[i].prevout] = CInPoint(&mapTx[hash], i);
        nTransactionsUpdated++;
    }
    return true;
}


bool CTxMemPool::remove(CTransaction &tx)
{
    // Remove transaction from memory pool
    {
        LOCK(cs);
        uint256 hash = tx.GetHash();
        if (mapTx.count(hash))
        {
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
                mapNextTx.erase(txin.prevout);
            mapTx.erase(hash);
            nTransactionsUpdated++;
        }
    }
    return true;
}

void CTxMemPool::clear()
{
    LOCK(cs);
    mapTx.clear();
    mapNextTx.clear();
    ++nTransactionsUpdated;
}

void CTxMemPool::queryHashes(std::vector<uint256>& vtxid)
{
    vtxid.clear();

    LOCK(cs);
    vtxid.reserve(mapTx.size());
    for (map<uint256, CTransaction>::iterator mi = mapTx.begin(); mi != mapTx.end(); ++mi)
        vtxid.push_back((*mi).first);
}




int CMerkleTx::GetDepthInMainChain(CBlockIndex* &pindexRet) const
{
    if (hashBlock == 0 || nIndex == -1)
        return 0;

    // Find the block it claims to be in
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;

    // Make sure the merkle branch connects to this block
    if (!fMerkleVerified)
    {
        if (CBlock::CheckMerkleBranch(GetHash(), vMerkleBranch, nIndex) != pindex->hashMerkleRoot)
            return 0;
        fMerkleVerified = true;
    }

    pindexRet = pindex;
    return pindexBest->nHeight - pindex->nHeight + 1;
}


int CMerkleTx::GetBlocksToMaturity() const
{
    if (!IsCoinBase())
        return 0;
    return max(0, (COINBASE_MATURITY+20) - GetDepthInMainChain());
}


bool CMerkleTx::AcceptToMemoryPool(CTxDB& txdb, bool fCheckInputs)
{
    if (fClient)
    {
        if (!IsInMainChain() && !ClientConnectInputs( txdb ) )
            return false;
        return CTransaction::AcceptToMemoryPool(txdb, false);
    }
    else
    {
        return CTransaction::AcceptToMemoryPool(txdb, fCheckInputs);
    }
}

bool CMerkleTx::AcceptToMemoryPool()
{
    CTxDB txdb("r");
    return AcceptToMemoryPool(txdb);
}



bool CWalletTx::AcceptWalletTransaction(CTxDB& txdb, bool fCheckInputs)
{

    {
        LOCK(mempool.cs);
        // Add previous supporting transactions first
        BOOST_FOREACH(CMerkleTx& tx, vtxPrev)
        {
            if (!tx.IsCoinBase())
            {
                uint256 hash = tx.GetHash();
                if (!mempool.exists(hash) && !txdb.ContainsTx(hash))
                    tx.AcceptToMemoryPool(txdb, fCheckInputs);
            }
        }
        return AcceptToMemoryPool(txdb, fCheckInputs);
    }
    return false;
}

bool CWalletTx::AcceptWalletTransaction()
{
    CTxDB txdb("r");
    return AcceptWalletTransaction(txdb);
}

int CTxIndex::GetDepthInMainChain() const
{
    // Read block header
    CBlock block;
    if (!block.ReadFromDisk(pos.nFile, pos.nBlockPos, false))
        return 0;
    // Find the block in the index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(block.GetHash());
    if (mi == mapBlockIndex.end())
        return 0;
    CBlockIndex* pindex = (*mi).second;
    if (!pindex || !pindex->IsInMainChain())
        return 0;
    return 1 + nBestHeight - pindex->nHeight;
}

// Return transaction in tx, and if it was found inside a block, its hash is placed in hashBlock
bool GetTransaction(const uint256 &hash, CTransaction &tx, uint256 &hashBlock)
{
    {
        LOCK(cs_main);
        {
            LOCK(mempool.cs);
            if (mempool.exists(hash))
            {
                tx = mempool.lookup(hash);
                return true;
            }
        }
        CTxDB txdb("r");
        CTxIndex txindex;
        if (tx.ReadFromDisk(txdb, COutPoint(hash, 0), txindex))
        {
            CBlock block;
            if (block.ReadFromDisk(txindex.pos.nFile, txindex.pos.nBlockPos, false))
                hashBlock = block.GetHash();
            return true;
        }
    }
    return false;
}








//////////////////////////////////////////////////////////////////////////////
//
// CBlock and CBlockIndex
//

static CBlockIndex* pblockindexFBBHLast;
CBlockIndex* FindBlockByHeight(int nHeight)
{
    CBlockIndex *pblockindex;
    if (nHeight < nBestHeight / 2)
        pblockindex = pindexGenesisBlock;
    else
        pblockindex = pindexBest;
    if (pblockindexFBBHLast && abs(nHeight - pblockindex->nHeight) > abs(nHeight - pblockindexFBBHLast->nHeight))
        pblockindex = pblockindexFBBHLast;
    while (pblockindex->nHeight > nHeight)
        pblockindex = pblockindex->pprev;
    while (pblockindex->nHeight < nHeight)
        pblockindex = pblockindex->pnext;
    pblockindexFBBHLast = pblockindex;
    return pblockindex;
}

bool CBlock::ReadFromDisk(const CBlockIndex* pindex, bool fReadTransactions)
{
    if (!fReadTransactions)
    {
        *this = pindex->GetBlockHeader();
        return true;
    }
    if (!ReadFromDisk(pindex->nFile, pindex->nBlockPos, fReadTransactions))
        return false;
    if (GetHash() != pindex->GetBlockHash())
        return error("CBlock::ReadFromDisk() : GetHash() doesn't match index");
    return true;
}

uint256 static GetOrphanRoot(const CBlock* pblock)
{
    // Work back to the first block in the orphan chain
    while (mapOrphanBlocks.count(pblock->hashPrevBlock))
        pblock = mapOrphanBlocks[pblock->hashPrevBlock];
    return pblock->GetHash();
}

typedef boost::tuple<mpz, CTxDestination> CBudgetEntry;
typedef boost::tuple<mpq, std::vector<CBudgetEntry> > CBudget;

void static ApplyBudget(const mpq& qAmount, const CBudget& budget,
                        std::map<CTxDestination, mpq>& mapBudgetRet)
{
    const std::vector<CBudgetEntry>& vBudgetEntries = boost::get<1>(budget);

    mpz zWeightTotal = 0;
    BOOST_FOREACH(const CBudgetEntry &entry, vBudgetEntries)
        zWeightTotal += boost::get<0>(entry);

    BOOST_FOREACH(const CBudgetEntry &entry, vBudgetEntries) {
        mpq tmp = qAmount;
        tmp *= boost::get<0>(budget);
        tmp *= boost::get<0>(entry);
        tmp /= zWeightTotal;
        mapBudgetRet[boost::get<1>(entry)] += tmp;
    }

    std::map<CTxDestination, mpq>::iterator itr = mapBudgetRet.begin();
    while (itr != mapBudgetRet.end())
        if (itr->second <= 0)
            mapBudgetRet.erase(itr++);
        else
            ++itr;
}

bool static VerifyBudget(const std::map<CTxDestination, mpq>& mapBudget,
                         const std::vector<CTransaction>& vtx, int nBlockHeight)
{
    std::map<CTxDestination, mpq> mapActuals;

    CTxDestination address;
    BOOST_FOREACH(const CTransaction &tx, vtx)
        BOOST_FOREACH(const CTxOut &txout, tx.vout)
            if (ExtractDestination(txout.scriptPubKey, address))
                mapActuals[address] += GetPresentValue(tx, txout, nBlockHeight);

    std::map<CTxDestination, mpq>::const_iterator itr;
    for (itr = mapBudget.begin(); itr != mapBudget.end(); ++itr) {
        if (itr->second <= 0)
            continue;

        if (!mapActuals.count(itr->first))
            return error("VerifyBudget() : missing budget entry");

        if (mapActuals[itr->first] < itr->second)
            return error("VerifyBudget() : got %s for line-item, expected %s", FormatMoney(mapActuals[itr->first]).c_str(), FormatMoney(itr->second).c_str());
    }

    return true;
}

mpq static GetInitialDistributionAmount(int nHeight)
{
    mpq nSubsidy = 0;
    if ( !fTestNet && nHeight < DIFF_FILTER_THRESHOLD ) {
        mpz zInitialSubsidy = INITIAL_SUBSIDY.get_num();
        if ( nHeight < EQ_HEIGHT )
            nSubsidy = TITHE_AMOUNT + (EQ_HEIGHT-nHeight) * zInitialSubsidy / EQ_HEIGHT;
    }
    else {
        if ( nHeight < EQ_HEIGHT )
            nSubsidy = TITHE_AMOUNT + (EQ_HEIGHT-nHeight) * INITIAL_SUBSIDY / EQ_HEIGHT;
    }
    return nSubsidy;
}

CBudget static GetInitialDistributionBudget(int nHeight)
{
    static CXcoinAddress vAddresses[320] = {
        CXcoinAddress("1DCyWRmTXB9goqA4Zb88nU1Q8snA7d7n4x"),
        CXcoinAddress("1LoFvV5YJsSMkpyPLizqyWH8KAkevV2XwJ"),
        CXcoinAddress("1JTUD2rB3FvbNFPw7cvCdTVDM9nuZTw7Jk"),
        CXcoinAddress("18w4xQQj2iXwtq9smYkEAJrWVz4jQNU4xd"),
        CXcoinAddress("16vdGLyxdYgSCT9xAng9Js7KrsnrUHsyG2"),
        CXcoinAddress("1Lo8mmskrLnvCuthadVaRS4K7WUSFpWAwj"),
        CXcoinAddress("1J1irQQ3ZWoTPct989Nnzdtu6WjfCjQcWs"),
        CXcoinAddress("1MME2u4V2ZiU6uUVJXTZMg5sQXAyMBUNXt"),
        CXcoinAddress("1CT3kUDi3rvma8R7Jwbz7puATSU3xzfLHz"),
        CXcoinAddress("1CLupi58K9XHVeWZ8jwbWiY4Ns46mPALbe"),
        CXcoinAddress("16A8XoWWvtJrDE1AdYQoLxAQcoLQML9gjz"),
        CXcoinAddress("1NwgZoUnudfmbQ99xDRdvrYskgjQ7KBt1Z"),
        CXcoinAddress("17CDPam7M59JM6vK5xzh1vUGKjYT9Byi5S"),
        CXcoinAddress("1PyKZKfquWcu3PFzKbvmKZ2oJWXbmbsWdB"),
        CXcoinAddress("186LbdeaDsn4Y5zrLN9cfSHWpQPSHtLbgC"),
        CXcoinAddress("1MrQWWNKfVseYyGkyyLsDhFekJWGJNt2i9"),
        CXcoinAddress("1EAUtv6YfvcRUrU5SncdZ27aSJ6SBNJH67"),
        CXcoinAddress("1DuSbRKB1GL9cBeJLYsuh3DADdwJgvHAQN"),
        CXcoinAddress("1MPdcnGXHsjR6rFSBUMm4ui44q8Ra1fYRT"),
        CXcoinAddress("1Ntv6bDFj8eQnXjawcatnJjJTowo1BA8rF"),
        CXcoinAddress("14j9vnqn6FZwPZmwdvGSuESm1m3oQsHP5y"),
        CXcoinAddress("1C679HkKyki9rN8tJvtMNyXGLedPdo8zbb"),
        CXcoinAddress("1EMKZYHTcnpHVUJx4dUp5Jne2ePQKjpdTm"),
        CXcoinAddress("1PmgFAV835znVpUwGkLkvJrKc4ZzBqixNX"),
        CXcoinAddress("16zKbgjQDqua6xjrXLhCbPGFrpr8UJxf4x"),
        CXcoinAddress("1KPurbuUH5D6HRe3Y148kUbRjDyFCCm3VH"),
        CXcoinAddress("1GVyWAXxP9tgZbj8iDSQqQ5tcN36uJ3F1s"),
        CXcoinAddress("1E5udyBXuBt1e8c2R27AvSTdp8H7LEhmxr"),
        CXcoinAddress("1hQcLTTD7KiFxiojvSrrrj8Y1w2gF5bHE"),
        CXcoinAddress("17BJ1oZdZJS64curVAL6rN1yYN7YiNVXpR"),
        CXcoinAddress("1LotiV7qGfAZhVV36XtrixnEfHCiuqe39e"),
        CXcoinAddress("1Q5yedqC3adLpNjbY4CWMxPojoxnSCVGjw"),
        CXcoinAddress("1FpBGhBWn7WDZr9nP47qG3DktJbaY7P48P"),
        CXcoinAddress("1H6Nh8dRPZjMm3KViuW5ZESjRwqYnQ36nt"),
        CXcoinAddress("1NAAKtpk7VRRUtA5ja8YxCZQaisXQ28HqA"),
        CXcoinAddress("1JKxed9uYfvcPgjGdo1GQXwMQJkAnap34G"),
        CXcoinAddress("1DZ58aSGD13QfUa118rtvfKrJiVPAoxdV8"),
        CXcoinAddress("12wnNuaQHbLyThJVJvfePhV8UwQEWURLLP"),
        CXcoinAddress("16f8S6f6ZDX3N1JG2DL5kyz9KCzmwpGgt7"),
        CXcoinAddress("1PPKwAUZ6g5wWiopfyJKJZn3xUFcrJbSBF"),
        CXcoinAddress("18DzCPRpU1Y2o5FsuuvcScZaYSi2ZBTVFr"),
        CXcoinAddress("1E5fy7csgbN5G9ENRwvSwGSAibLdLk52pe"),
        CXcoinAddress("1Dapd3WLAz1jm91FpNThHamXeMjDU4TJgJ"),
        CXcoinAddress("15HQuReQzSQ1mrHWy3iYELyJLjGNe9gNEZ"),
        CXcoinAddress("1DcJhNQJLkDrSmrvATciEaf95ZvnhFFUF7"),
        CXcoinAddress("1ErNVYRnGQpzFmxkXYnqR4LbcCViby7Rfi"),
        CXcoinAddress("1D1CmGn3BCM5rviTxZEfc7NhozAetePkit"),
        CXcoinAddress("1Af3dbEWMK5VuMkUozepYPQgMeVtmKtvW9"),
        CXcoinAddress("1JY2W5m4jsYzY2YYXU6RRKDmobE3BYEbgA"),
        CXcoinAddress("1PdTBBm2xhCoUY4A6cfYCopaFDsFyTf4MY"),
        CXcoinAddress("1Fe51wUzrhyGmag9UXmzEsr6jSyWqcATAM"),
        CXcoinAddress("1kyb1A5jWYP49YTkoN2y3JFQuNp1S2gXa"),
        CXcoinAddress("1FxZ7fmDQmauMASYVuVcHeajGZQKrQ1UWB"),
        CXcoinAddress("1EwtDpNLPmUZNLFmGMmNTwviUVe3DuTFKt"),
        CXcoinAddress("1NYRPya8KWUfiSr8fXxccPoDMmBw2Uqj1y"),
        CXcoinAddress("16vQMSBZK7iy5HDFfeiP2WomfpGfSEPJx5"),
        CXcoinAddress("12E9bCLYb9uzh2MHhpsyR89V3eLXZp5afr"),
        CXcoinAddress("1EA4NJjMXSgVNsNgEc7nSyRf3epjp3ABrQ"),
        CXcoinAddress("1NN442B74LAsXUMUFZSriWZCUh8b5ECFR9"),
        CXcoinAddress("1EMaEQmjjDCjgu3auEam5ABQ1J9ZtdLdpV"),
        CXcoinAddress("1RYXoGz2cHTGsYC5zZdDwpCdGRj4aBdAX"),
        CXcoinAddress("19aDWt7kBf53uLANiWnLFnWo5CqASh79mi"),
        CXcoinAddress("1LDEniSxXknXLHT1BMWpFsBM3PQcgn1nYz"),
        CXcoinAddress("1Q4Lji94eWCC9xBzwrbRE9yTMYS5fdKg9z"),
        CXcoinAddress("16fQVYur5CVMq9VfNLYypKXNeTmvWnDKsz"),
        CXcoinAddress("1Mc3r8pCpuRiHhkD3DrWf89CUnZb6xbFbg"),
        CXcoinAddress("18oEnf5iR9CD2HFDc9Yr8kD7m5CrJVWRkv"),
        CXcoinAddress("12VDq99L8UQWr8Waqo4GreEGCEBnkxMaXy"),
        CXcoinAddress("1H7PxhMmvqiRT8NDEkSFjfDekRLQ65CqBN"),
        CXcoinAddress("17yC59RcpYsw7jX3Zw7c48AcWtJqaHUwAr"),
        CXcoinAddress("1AFT16ksWdqdjhk56gFDaRnr7vS4XCVtyQ"),
        CXcoinAddress("1G84MZVqN54QTD47YWWmimy9htaj1WC58U"),
        CXcoinAddress("13YcisL6YyUG5nqegyqyrL6pVtrMqGYtcq"),
        CXcoinAddress("1NYdmagVHfbqTgW4hYJKS2YnWrJzCnSsvZ"),
        CXcoinAddress("1PumqgHPLUjPKfddgwJA46D5GBdYgT8myg"),
        CXcoinAddress("1PFKxU6g1kQayDwvpiLX2vJgUghMqJz9Ck"),
        CXcoinAddress("154ENKy3HuYoN8xARVaxp61NUAt5GEknDj"),
        CXcoinAddress("12CJ8BD8L8tQXjrpy4UfjJwCCtoL6vsegD"),
        CXcoinAddress("1LXCWYJ6k7EG2Bi8rLh2jhV94L4G768yTa"),
        CXcoinAddress("1K1rbcUFmE7XScTsqiNEiJHyX69eqbZdDB"),
        CXcoinAddress("1uZZzXiu8n7eL96rcFWh9MvcqerYxaGce"),
        CXcoinAddress("1JidqtE1YHwXFC1utxPAp17RkM3rUqwULk"),
        CXcoinAddress("1PuMwPqNLLYi1sPxvJToid2EsfiP4xPfo6"),
        CXcoinAddress("14ZSJRvSdgYFA1xUM2txnQKdMXMfsEWvuJ"),
        CXcoinAddress("1D9RJw7p5zgz4JeWvVzYxBsAkvucRMiXfG"),
        CXcoinAddress("1JRpRLZgcfNNeVEwGQmYZw5nv7Aq3KVx5x"),
        CXcoinAddress("17Rqyx39YnpFN23dPE3CWRPC8JhuBVKktx"),
        CXcoinAddress("19pozj4JeWd6rpMDeTpx8d1Dv4rebhUkvT"),
        CXcoinAddress("15jULtTPTzXHr9ezTMFbaPJojbuYFrbrQp"),
        CXcoinAddress("19dfCSTERPh5j4XtYoJatjdjD9afReeY3s"),
        CXcoinAddress("1LgzNc1Sfbu8BaxKUESGbNzCNnpqvhpCi4"),
        CXcoinAddress("1HTvoZUUNncPkjjv17xHLEtncdrgcdnN46"),
        CXcoinAddress("1NHvSZWwk8RtgPvfhzykpvebQnVk1Q5XxX"),
        CXcoinAddress("1AzdeDfjz5C5yT6wVxurgS8QPkZviHvY8N"),
        CXcoinAddress("1BdFwnfS84uDeZn4sojUs5ZC8fSkx9o2XG"),
        CXcoinAddress("1AgCAgvQZPQTkdMg853SkM2WdRzN4Q2ATw"),
        CXcoinAddress("1JABYERsgkAYincsgCpic7MwV63iM19iXp"),
        CXcoinAddress("1JFudqZDUkBMdV4ShLmhxLD7sfNEYdBQCE"),
        CXcoinAddress("1Pqf48Skyxt77RNVwTLxUhA2BNCscaHJKa"),
        CXcoinAddress("1AtdTwFFYZJrUUSWbBLBCkodRcnqwb1a6G"),
        CXcoinAddress("14iezrH1nR9TjGtnywFPqBHbwYcEhwz8y9"),
        CXcoinAddress("16x2aavFb2AHKntUnzA3HC2wmi921YJn4i"),
        CXcoinAddress("1HovjtiToM6f2xV3Sxg4fxfvSYPCGGEXLe"),
        CXcoinAddress("1MNrqZyo7poywLPVap6PsmmT5CS4f8hyWq"),
        CXcoinAddress("1F6PzQRW2MPfCYvzgeUXoBXaEikH3E5zMk"),
        CXcoinAddress("1F2SpgUakBvx6aNgJiCtEZHnTqVWeQcoMk"),
        CXcoinAddress("13iTRwxSLGC17fzumSrRidaXe8v8awdDux"),
        CXcoinAddress("1KuyBiZBdXVq8oNGAPWEqWiFi2RyH8rvwd"),
        CXcoinAddress("1HdXmhHKkkzpn1UKmhBWFzQMYsUqxUuVZ9"),
        CXcoinAddress("1Dw9jXoWc5MsEH3uLB9pi98qeyijUrvWU3"),
        CXcoinAddress("15mW5WsusPo6LAAYLqa6ngFfQ1jX51v3Bn"),
        CXcoinAddress("1DFfarcjskvSi2w56msV4JeeVZqtuwEL9p"),
        CXcoinAddress("12SeGWd2txi4fdQKoFXsTdd2fgjDbABWyb"),
        CXcoinAddress("1MoENmjtakS8XTHcwsbVFeJkjEckMhS3xm"),
        CXcoinAddress("167pv4Hn53XQ4hFhyNtEyP36n8HrL3NU3j"),
        CXcoinAddress("1E6WgpC4bmYJagvsTzhRxZ1Z8sRSsQjmJX"),
        CXcoinAddress("1EFkVCzezsZCq56JWSBRf3Dy6tafFRxh4N"),
        CXcoinAddress("1KnKZwDb44Qf3Lutda2T85uFZiTZwe2v2C"),
        CXcoinAddress("1CLpF2fLukzBHard43mXLEXxz11gFK5dc9"),
        CXcoinAddress("1DXSfPi1Tj6tQ5qf5M6Yj6cpNmLfPKMwr1"),
        CXcoinAddress("16nHP74UsqeHewM1yUhNCL3zCjkWnqFt8g"),
        CXcoinAddress("1FeqXkG9jGEDcPaKJV8rdh4NbqTjbdvN4a"),
        CXcoinAddress("1LwwjmsoDtQ1Zh9N8doGMczP1TJnes2YoZ"),
        CXcoinAddress("1DgusdNgB6nRD2emfwURMmk33LrB7Wp95c"),
        CXcoinAddress("17kjPofVVmhZAWXnrVwfqizGtXWBufWwbf"),
        CXcoinAddress("1EnLHA3U15wXehXAC24W587EEaeyUcaA6K"),
        CXcoinAddress("1MwpkFtEwrAQNbsmbt4kB9WtoB8mFLXZ44"),
        CXcoinAddress("12iQRcVoRCbFNvoQARM3rufTkd7jXpHZEm"),
        CXcoinAddress("19zK2WFDkaHZfWa4uS5mzF2XD1KrZEMxy2"),
        CXcoinAddress("12Zs8LtRY1cTS3HKw1gwPzYjB1Ar6Er93R"),
        CXcoinAddress("1KDVcQhjZuX39Fvv8QbrSpaSycMA4YdPkU"),
        CXcoinAddress("1AT6rxNBT8sasYKrKm9fv7LdjXBS89Wewh"),
        CXcoinAddress("19YjbLEUgqV8joQMgijDWZoY1inwXf1hXc"),
        CXcoinAddress("1EpHQ43BkzmKYMiYwmRRKEXQidpgA499px"),
        CXcoinAddress("13bQP93mmUFtUGVuBEwZ9ymdbCC9yywgdL"),
        CXcoinAddress("1GatPyGkCX5YUW4f2QHJk1PzwspCRz9b3J"),
        CXcoinAddress("1Jk8sCUfHVE6VpwkkTG9qaYYS9u1zMmQAs"),
        CXcoinAddress("13N4Eiv2KiX4PeFwiWnC847JBv4TP2sn1Z"),
        CXcoinAddress("1ESzED9saJ3bVB6BbVSTFGDxRLnTgWRVDC"),
        CXcoinAddress("1CspvzG7HyuNXRLsaWnpsLXPDwkeDKd4mm"),
        CXcoinAddress("14Rs4fo9tK39kyEFoAjbvkcgGZ6k356t3T"),
        CXcoinAddress("1D6jgPJYoFhbY7gJjNMAbyfJzBGVtqSc1o"),
        CXcoinAddress("16MHoaVyYQgPU525fz2auJpK6JVyFKEiz1"),
        CXcoinAddress("1FfS9TQswYZHYDNkUmncRAYjYJkLzGncp5"),
        CXcoinAddress("14PXPSEjNjWAuqYa63RBT6gewnomE9saRu"),
        CXcoinAddress("1CHBBtBCRQz1TFyE12g8RbGPZ6UzX2AieC"),
        CXcoinAddress("1CMwT1jzfoe9VvURpZanaXVQobMQLr13W8"),
        CXcoinAddress("1696KNrMvHvnthPLZnGuYGY96UbEqLeXz6"),
        CXcoinAddress("1A7TQi9sMiNQX8uwwqFb8eqaXnpTJY4WYg"),
        CXcoinAddress("1GNuX6AN2KCF1AWtxAT9QYD6QRJubRvKaz"),
        CXcoinAddress("16L3CvHeZcZcr3wPhoEC3ZsMLN7YTonMTQ"),
        CXcoinAddress("1EnqRdqx1VZyfc5ia4pcmZstBcGdW8FGxn"),
        CXcoinAddress("1MQ1QeCMZhxFCgReGEPRS2Qy74FaPqFccW"),
        CXcoinAddress("16Za6Rn8dCmM8gctXQtwN1yQ2WXnhHsSgs"),
        CXcoinAddress("15k38dy86CRnirMY9Q1niVmfn7nfXTmppL"),
        CXcoinAddress("1MQsruCXBjCZzTZKKpPwcC74ztetbtAw4E"),
        CXcoinAddress("1cmAt63c4ZAqRe2fBQTYs5Jyx41fiBbhQ"),
        CXcoinAddress("1PBCaowV7gQM6Lj1NfSpH2TnHHmqXcYTsC"),
        CXcoinAddress("1841uXFc2kUTUogCDJwp4U1NPjSPqsg69x"),
        CXcoinAddress("1XNo9kDMM6uqvf9yCWmqj17rukC8abjtb"),
        CXcoinAddress("1A4kHAe6rNz1q8G6dYjNMyWzgVv4DxYget"),
        CXcoinAddress("18Y6y6zcJrG5j2RjmGqsUvtWkZhnTvRka7"),
        CXcoinAddress("1DkcMkHWUUVjXgAu2MFXVkUuwZ6JWv64cz"),
        CXcoinAddress("1JvXTyBxhjE9mERWEFnqeuAPgbJSi25qGd"),
        CXcoinAddress("1FHus2MsM8k4oKHt22YFYeoFkf65kxQFP3"),
        CXcoinAddress("18HLkAhrzeNsaMB3MY1xUGW7wkzjWGobT7"),
        CXcoinAddress("1AF7KmTRrS3mMxop2Viop1MctrNJmPAHQt"),
        CXcoinAddress("13g4rWjU2PK4eN9D9XXo4jRB84RiJ2hD7o"),
        CXcoinAddress("1EWUiUoxZXfTbZXDZGueag7XRnv5Mej8ZZ"),
        CXcoinAddress("1LuuGk4tyd4USQqtYypemjt5vs3VRqV1QU"),
        CXcoinAddress("15eUUDUYDuiKnt9xNbzhNFmorCK9F9mJb2"),
        CXcoinAddress("1HGzWgdrNAKsE9nE1GHtUvaXHNzvwTyPQX"),
        CXcoinAddress("11MhmCVmFszm6yTTwaK2dypwcLaybmCjp"),
        CXcoinAddress("1s9XWpGPQqhbog1S6xgGqcVnfvnLMAueZ"),
        CXcoinAddress("16f3tHcuRavx3tSWCM2jnnCX5jGa2vJe9Q"),
        CXcoinAddress("15NWaghRx51ravYTUqsnBF2hQFQeSHtTvS"),
        CXcoinAddress("1QGUUgikmqCinDQn3vfqx9q6mnT5ekA4BG"),
        CXcoinAddress("134WvpvyZUveYc98CmtWZc1oBBXdrV1GuU"),
        CXcoinAddress("1LqNfcDBn7eytc7Ln6fLrCDLkYeMa6R9dV"),
        CXcoinAddress("14xbponjm6rXp8cNzTJmtCJwvwvDuKvaCD"),
        CXcoinAddress("14D7JyUrv1HeSD7FCc8WupmbxUiGyfC7uC"),
        CXcoinAddress("1ER6GhDJokhBjB73DWDTdC2BP2J9DiqD1o"),
        CXcoinAddress("13Q3or3Hew7hBZzMoriz8LcMXwptqD5HEd"),
        CXcoinAddress("1HSyeVQEvdRwj2rutFN33cKu2tPzyGkgx2"),
        CXcoinAddress("1A1WaQQ6ZjXuEe2KYZNC3ycPg4X9czsR4D"),
        CXcoinAddress("1fhzxkMPY4hUYNywoQwyVGkinVKQrPJ2P"),
        CXcoinAddress("1Nf63BqwEmb7vU15bRfpvKEs5tMGZpR5Fi"),
        CXcoinAddress("1Gi6tjnRBecQovhRQVNmsPyVZYmphZerdg"),
        CXcoinAddress("1AJ87nhgSQkac9BUjEvbyWh8c95ciHLZWG"),
        CXcoinAddress("1WDyJLrJaLRePMtea6bAgADwzdpbW5nqd"),
        CXcoinAddress("1JTvhcJuxydevXw4ocUUteiPNWwPtMM56H"),
        CXcoinAddress("16X1LYmpxM2fPBjNTLbnPo2LdA6sB7fbNu"),
        CXcoinAddress("1315ZWhxgd6pqqTmvF21fxt5wzYvpcnZSm"),
        CXcoinAddress("17PWpyrUmkaCVPu6KXaWvuLLYvD9YU61RP"),
        CXcoinAddress("1PADxQpcx8Kvs3PprjYvM1wYFyjxB3tcs8"),
        CXcoinAddress("1BWyJmxybx3p1guhud8qxabrGbVLWaVNaM"),
        CXcoinAddress("17JpmLSEbXgmheAvTQ7iiBvR5TaSsM2Xgt"),
        CXcoinAddress("19oxMuyyipVsvxXWKBBrFmY8hQbWkiiVEv"),
        CXcoinAddress("16TvroBFWJmUN7VSHQLmyh6KiCri5QVTQu"),
        CXcoinAddress("1MXJR6XRoThY9rwvyvLkXWN17WN7rAQC4J"),
        CXcoinAddress("163N8CmDAf45CM6brXMpzg3AN2nkDXTuRt"),
        CXcoinAddress("1CWudCKLCxT5AXteLFeZRBDyb4moQH4cVL"),
        CXcoinAddress("1DYmgt2zpW2eNfyczC98aq76URHQMnfwZK"),
        CXcoinAddress("1JnE2YseXgBX4oHGo8VywsxnNkp52s6nkX"),
        CXcoinAddress("1D8WBBBCHhgLrMa8s3QU1bkRcRHEt8cNfv"),
        CXcoinAddress("1Fm5eoDvEZo4hyW4YEDu3q2gKbpCuo9hqw"),
        CXcoinAddress("18uRTixnVaKMz9tyoR6Ve6Rqdwtt8oZ1Zw"),
        CXcoinAddress("1FuByKdd2RK3hjc3UFeV56HvheyAMnjMMS"),
        CXcoinAddress("17nDqatJ7M6M9vFRa4BngCCLPGSJ6mfc8b"),
        CXcoinAddress("1G4qHkiaaVZwuLqwvh2itFjR18iThkeaDQ"),
        CXcoinAddress("18ZcHUg5wV4sSdd9pS7xv5rYsfx5D1hZWi"),
        CXcoinAddress("1CZU6UCZjtWueXQWYzyrFa4K7pTSeBQ8cw"),
        CXcoinAddress("19zRVJvXaXZvygqbHAP1ZKF5Rx9gq3Xh8u"),
        CXcoinAddress("1HQAyw9UUi2eiQHJcnbg5eeJTnv2QoEQqA"),
        CXcoinAddress("16eZAqdqypn47T8DwS1archd39uXqK8JQ8"),
        CXcoinAddress("19he5Hy915MbSZBvwHjB3LAm5UyLnmQ5TK"),
        CXcoinAddress("1CzGcY5JKDroUtdFdZJArGeEmKMEtyeAKw"),
        CXcoinAddress("1DzowkZrtEQgoDF8xgxjPBfLaBMeBHjNr2"),
        CXcoinAddress("16GA6zc9iTUB8o47oi7fbE88ayEi8C7w2r"),
        CXcoinAddress("1Mvp5TikHrzJetDMbjHkzAkP9rMBfQrais"),
        CXcoinAddress("1Lrbk1vrmCqVfajBqtwHD1x9x72jeDCon5"),
        CXcoinAddress("1AMDHRKUah3J8yESFt7NnoUXrM4ULHcUpN"),
        CXcoinAddress("1MbnTTv5FJX8RsK5tw9KjNx1VCvo94GEKK"),
        CXcoinAddress("1Hmbm1TUDuDwdVWkU1oiaReRRBTzb8fMDJ"),
        CXcoinAddress("15XYapuYSjaDc4uDXJsf3PF33YzSRs5P3M"),
        CXcoinAddress("1C3ovhhZwo73isNQPuKKD5VDm2XwByBkTK"),
        CXcoinAddress("16VJrBFjFjhLY93NihDvWqBpUeiXeL2FUi"),
        CXcoinAddress("1C9Niuy1cSW6a6g5tm8GhPsSML6ZtWeUQS"),
        CXcoinAddress("149937wZtsTvtwmixD33npnsnyUm5zjstX"),
        CXcoinAddress("1NkCKjPZUFecVWxLGnJbN7Fp8viJRG5Xg4"),
        CXcoinAddress("1MG3okwhF3YDwVWDcYsNr5ySA4eMtCATrK"),
        CXcoinAddress("1GtzbwNuHYBZaDRVpJGuDwjBQhSh5RBVRZ"),
        CXcoinAddress("1CJi6dja55AtGeuJX6WLFGTHsoofqZyDNu"),
        CXcoinAddress("1oftVXkjfpJSMKGnz1pps1xVWNUNNhAmq"),
        CXcoinAddress("168KgGGUEEx22eCNuSMjsKvn5chiZ5c217"),
        CXcoinAddress("1FmQzGLJFu3AvucwDEAjYRM4fPgiSZsT6Q"),
        CXcoinAddress("1F9t4EmWXy2Wui21LaMuZmRDwRCF38aDZN"),
        CXcoinAddress("19eFuss1dgxPdDfoAu6AsVmBUj5d2DUPu3"),
        CXcoinAddress("15Yr2PPbFGqbi2SZtZ59cvd5y2Es8atRE5"),
        CXcoinAddress("1Pz8oisCda5aJXtVVDo1mfxxvgymVNcmsM"),
        CXcoinAddress("1HkHQHNkjXp6VinoEG6a1i55NmreXC9yAX"),
        CXcoinAddress("12ShPmbsADZMacnr5u2DPxssKXjd3HaCZc"),
        CXcoinAddress("1BasQDDfZ677LF3mEAQUEFHvJexZ8ZxY47"),
        CXcoinAddress("1FVXTVaK3rwSrx67WGdNkNFwL5sVm81TEK"),
        CXcoinAddress("1NV8VjVBrkgCTJvyBHZumboXjPtSNZvRJX"),
        CXcoinAddress("1HVdN1BSusJZ42sSfJFHB2CJ6LcW5Fz31a"),
        CXcoinAddress("12j2yfUP2dNo3HwdrTDjMGZhzBcdhYvFj6"),
        CXcoinAddress("1BrWHBKCpvNYssq8Kj8yY6qvy7GqFgk8UP"),
        CXcoinAddress("1G3faUnBxMHwwX2uLn6dZJEj9pmJ2o5cnq"),
        CXcoinAddress("1Nq59Py1u73B26aTTRhZG3g9h5fmrmkeX9"),
        CXcoinAddress("1DMuz7B193myzVq4Kgg76Jb6Da2UjkAti"),
        CXcoinAddress("1124BMmAevhic3H1MQB3teQFhoVi7RVUhR"),
        CXcoinAddress("1B1hrgcDNfSuaKJi3oJ4cBtyysq2BpGFz8"),
        CXcoinAddress("18NE1w2soK6xGUYYvoTe7oEtRtQhxBLXCq"),
        CXcoinAddress("1MESy7CY2yTgxSERyejcvCGjK8Qm2EhE4g"),
        CXcoinAddress("1445Hs1Lgh9pPvD8mSt5oiGwTY8yT2sy9R"),
        CXcoinAddress("14pm2Fxwin4mwHqd5ujXAXTJJFuQ31qYUf"),
        CXcoinAddress("17UhQpeFQ3nCjj8PJKCrTWHnP8YSvrNM7h"),
        CXcoinAddress("15zVu5t8iURV2feuvmnHgYp9u7cxPC4XrN"),
        CXcoinAddress("19JriYALeNskNnvjYidpoNHNLegftkViqH"),
        CXcoinAddress("13mauBB6JYTPcfoWbNbCWKk4sNqmwxCXse"),
        CXcoinAddress("1LwRt9rpGaekbht7UAitg2ADmFtDrKThYV"),
        CXcoinAddress("15Z9wnxM6VxrRkqhLZpLskGRJ2dLRMEmCg"),
        CXcoinAddress("1GvQwfMSMRggmmFCRqf1EmvaG5U5sY4sKL"),
        CXcoinAddress("13iUoiiVq7C4fUmy94r1HEDf35YKwBAVXh"),
        CXcoinAddress("1ELZsnzgBmZSSxQQYuAANg8izDFTbzhbPB"),
        CXcoinAddress("13me2Z71XAtmkzggnqusdvuRiXZzFRGZBj"),
        CXcoinAddress("1KC7ECvdcofiYXJ63iUnvFrEH4zzhQZ2pB"),
        CXcoinAddress("1LtFLa3oaEBhmHQ5iXRvFqeNcrzU8GPNMM"),
        CXcoinAddress("1LNHH8DGXWQRyfmkSkaJcBkkiVxUhH8tBM"),
        CXcoinAddress("1Daw5kGzsqBhfRfMV6dAA4bgBZ2LBWS1nY"),
        CXcoinAddress("164XLENwRiappRPUP47sRTSyXtW8CAXVLi"),
        CXcoinAddress("1CFoTaknkFGADVo2rK92jwq18NWBzVcJGS"),
        CXcoinAddress("1MnrNuPrnuJFxYnkpKDqUymHGjb1d6qLVq"),
        CXcoinAddress("1BpwPwf8kUssmoMCoWnHCVY4wjBJi3CZyD"),
        CXcoinAddress("1jiJb2DU3DB6ujD8eV3DZXmnfwWaHti4y"),
        CXcoinAddress("1DqQnvWdtKvwBtePpCbDd8juZ9ZbeaKFdH"),
        CXcoinAddress("131D32PNpqqGtLGUaAaZePqpUdBTiy8Akh"),
        CXcoinAddress("16jK6KaY7Ub7fZ7YaBi94ZsygovzixnRNx"),
        CXcoinAddress("1EJx1ShX4UJVrzynP3oZw8wdLpSGC1KPrz"),
        CXcoinAddress("1899kFmma5FongtN9JfvFKqhwtbw2w9MDe"),
        CXcoinAddress("1FNNBK9SeDUufbbnmoagUFt7oKVbb65vaw"),
        CXcoinAddress("1LxF4pLjeSpNs4ux24MDduzCzrM2KCsE7M"),
        CXcoinAddress("1BZjVRe2CCA7G7qnG3beWWhG173f1mbNX2"),
        CXcoinAddress("1L5HWs7WrK457CzjAgnHghveFpQVv7rRTe"),
        CXcoinAddress("1Kp4bjG9nwbogd8WM62ijGG6onW9Wo4aYK"),
        CXcoinAddress("1hhvJ4QmB6RX12Bps9xhnMHCDDXTXAnDu"),
        CXcoinAddress("19xiuTYSm85gNsPZw8hGLS69e2DjVbCuAP"),
        CXcoinAddress("19WdcJU8Z1W3ZZnbpfDRbdrYGapxp1L5zo"),
        CXcoinAddress("121hT7w8DN3x1pYEowak7FjmNgihMNo2cd"),
        CXcoinAddress("1BtthjPb9GPKwgcJtrgZRQRWhiRSCHmyvk"),
        CXcoinAddress("1JKgRTkMgEodFFpPwoz9W6pejMN3x3J9X1"),
        CXcoinAddress("18zp4dHdouYqFn2qC4ttAva8cwqhZ4pm4K"),
        CXcoinAddress("1ELAVZKvGykuzRDCvFUsJTL4istYisbxpK"),
        CXcoinAddress("1PfULZdJniM8SutFdjoKvG3WLUwxZL2YUf"),
        CXcoinAddress("1DTbnuz4dPLdduseE3k5xr62eFAYjCSk3E"),
        CXcoinAddress("14jVGdWJRcqpdgWPAbbvhMfVnLha6MdnYU"),
        CXcoinAddress("1GBiMVjsqkcGxij2hGFQxVUX2WjDcr1Esf"),
        CXcoinAddress("1FGUuAuGRkSqEL8Besg33QsekxmBB75ZUH"),
        CXcoinAddress("19FsZeejdbfUKK21wENRdoR2BUowD4FsMZ"),
        CXcoinAddress("1LbBHWffmANdhcb1Wciv4jWwXPGrtVFhsU"),
        CXcoinAddress("1FT9PRuDmFKxZorYrfgibWaaBdKWv7PiB4"),
        CXcoinAddress("1FUvPJ3nXMUrFEWqkjxPe5esqQ2GoCmUAk"),
        CXcoinAddress("1LJLBDK8q7yLibK2oYTA6hbD9UpmP6U3QP"),
        CXcoinAddress("14E9GEg9T5N9aja1FV2ewNFjMK6wPEgsKb"),
        CXcoinAddress("1JYmABbYkUjAyowLwa1zoQj86PEWMBdeZP"),
        CXcoinAddress("1NCfTbrEsZrCT3Efyk5AfvqP2xY6NesWHy"),
        CXcoinAddress("1Bwd6rcgGLq8sdo3FHHSmh3J7ufqdgMeqi"),
        CXcoinAddress("1PzAWHEt2xabgWEki5hTgwtTyuKRS1at39"),
        CXcoinAddress("1Fo3r7DWDtJ8Yu2UqngNqKMSw98XgsXehW"),
        CXcoinAddress("1H1b6FLd5eqH8Q9Cw8UkZv2nY3xxKTfsH3"),
        CXcoinAddress("12x9TqiF9FQU7sqnRiCmrRZmG7dLs9hyG6"),
        CXcoinAddress("13VYYQ8K9AFiajev9QdHM6Kj8SqevRT7GS"),
        CXcoinAddress("149HFz2K7D4GffQm8t7rKQuWmcwJohsimk"),
        CXcoinAddress("1JqwkYTg3ZuWMpjhxrJYgW7E826HYoiBSG"),
        CXcoinAddress("1NiyjCKxM33nozwzU2LNtWBPWrWTUpiaAM"),
        CXcoinAddress("1LD4F5tA87e7nMwNRuHhgwH6zTFZ1LyoE2"),
        CXcoinAddress("1M3wUX9YYrcVSSw6Tncdoic3Fj13okQ63u"),
        CXcoinAddress("1PVKsqeVqM4B2ccq915GHeK3aDeruStr24"),
        CXcoinAddress("1PKNQqSuPknZ1PaqKkRqa9qYujWKL9KQ7E")
    };

    static bool fFirstRun = true;
    if ( fTestNet && fFirstRun ) {
        for (int i=0; i<320; ++i)
            vAddresses[i].ToggleTestnet();
        fFirstRun = false;
    }

    static CBudget emptyBudget = CBudget(0, std::vector<CBudgetEntry>());
    if ( nHeight >= EQ_HEIGHT )
        return emptyBudget;

    std::vector<CBudgetEntry> vBudgetEntries;
    vBudgetEntries.reserve(1);
    vBudgetEntries.push_back(CBudgetEntry(1, vAddresses[nHeight*320/EQ_HEIGHT].Get()));
    mpq qRatio = TITHE_AMOUNT / GetInitialDistributionAmount(nHeight);
    return CBudget(qRatio, vBudgetEntries);
}

mpq static GetPerpetualSubsidyAmount(int nHeight)
{
    return MPQ_MAX_MONEY / DEMURRAGE_RATE;
}

CBudget static GetPerpetualSubsidyBudget(int nHeight)
{
    static CBudget emptyBudget = CBudget(0, std::vector<CBudgetEntry>());
    return emptyBudget;
}

CBudget static GetTransactionFeeBudget(int nHeight)
{
    static CBudget emptyBudget = CBudget(0, std::vector<CBudgetEntry>());
    return emptyBudget;
}

mpq static GetBlockValue(int nHeight, const mpq& nFees)
{
    return GetInitialDistributionAmount(nHeight) +
           GetPerpetualSubsidyAmount(nHeight) + nFees;
}

static const int64 nTargetSpacing = 120;
static const int64 nOriginalInterval = 2016;
static const int64 nFilteredInterval =    9;
static const int64 nOriginalTargetTimespan = nOriginalInterval * nTargetSpacing; // two weeks
static const int64 nFilteredTargetTimespan = nFilteredInterval * nTargetSpacing; // 1.5 hrs

//
// minimum amount of work that could possibly be required nTime after
// minimum work required was nBase
//
unsigned int ComputeMinWork(unsigned int nBase, int64 nTime)
{
    // Testnet has min-difficulty blocks
    // after nTargetSpacing*2 time between blocks:
    if (fTestNet && nTime > nTargetSpacing*2)
        return bnProofOfWorkLimit.GetCompact();

    CBigNum bnResult;
    bnResult.SetCompact(nBase);
    while (nTime > 0 && bnResult < bnProofOfWorkLimit)
    {
        // Maximum 400% adjustment...
        bnResult *= 4;
        // ... in best-case exactly 4-times-normal target time
        nTime -= nOriginalTargetTimespan*4;
    }
    if (bnResult > bnProofOfWorkLimit)
        bnResult = bnProofOfWorkLimit;
    return bnResult.GetCompact();
}

unsigned int static GetNextWorkRequired(const CBlockIndex* pindexLast, const CBlock *pblock)
{
    static unsigned int nProofOfWorkLimit = bnProofOfWorkLimit.GetCompact();

    #define WINDOW 144
    static mpq kOne = mpq(1);
    static mpq kTwoToTheThirtyOne = mpq("2147483648");
    static mpq kGain = mpq(41, 400);       // 0.025
    static mpq kLimiterUp = mpq(211, 200); // 1.055
    static mpq kLimiterDown = mpq(200, 211);
    static mpq kTargetInterval = i64_to_mpq(nTargetSpacing);
    static int32_t kFilterCoeff[WINDOW] = {
         -845859,  -459003,  -573589,  -703227,  -848199, -1008841,
        -1183669, -1372046, -1573247, -1787578, -2011503, -2243311,
        -2482346, -2723079, -2964681, -3202200, -3432186, -3650186,
        -3851924, -4032122, -4185340, -4306430, -4389146, -4427786,
        -4416716, -4349289, -4220031, -4022692, -3751740, -3401468,
        -2966915, -2443070, -1825548, -1110759,  -295281,   623307,
         1646668,  2775970,  4011152,  5351560,  6795424,  8340274,
         9982332, 11717130, 13539111, 15441640, 17417389, 19457954,
        21554056, 23695744, 25872220, 28072119, 30283431, 32493814,
        34690317, 36859911, 38989360, 41065293, 43074548, 45004087,
        46841170, 48573558, 50189545, 51678076, 53028839, 54232505,
        55280554, 56165609, 56881415, 57422788, 57785876, 57968085,
        57968084, 57785876, 57422788, 56881415, 56165609, 55280554,
        54232505, 53028839, 51678076, 50189545, 48573558, 46841170,
        45004087, 43074548, 41065293, 38989360, 36859911, 34690317,
        32493814, 30283431, 28072119, 25872220, 23695744, 21554057,
        19457953, 17417389, 15441640, 13539111, 11717130,  9982332,
         8340274,  6795424,  5351560,  4011152,  2775970,  1646668,
          623307,  -295281, -1110759, -1825548, -2443070, -2966915,
        -3401468, -3751740, -4022692, -4220031, -4349289, -4416715,
        -4427787, -4389146, -4306430, -4185340, -4032122, -3851924,
        -3650186, -3432186, -3202200, -2964681, -2723079, -2482346,
        -2243311, -2011503, -1787578, -1573247, -1372046, -1183669,
        -1008841,  -848199,  -703227,  -573589,  -459003,  -845858
    };

    // Genesis block
    if (pindexLast == NULL)
        return nProofOfWorkLimit;

    // Special, one-time adjustment due to the "hash crash" of Apr/May 2013
    // which rushed the introduction of the new difficulty adjustment filter.
    // We adjust back to the difficulty prior to the last adjustment.
    if ( !fTestNet && pindexLast->nHeight==(DIFF_FILTER_THRESHOLD-1) )
        return 0x1b01c13a;

    bool fUseFilter =
         (fTestNet && pindexLast->nHeight>=(DIFF_FILTER_THRESHOLD_TESTNET-1)) ||
        (!fTestNet && pindexLast->nHeight>=(DIFF_FILTER_THRESHOLD-1));

    int64 nInterval       = nFilteredInterval;
    //int64 nTargetTimespan = nFilteredTargetTimespan;
    int64 nTargetTimespan = 1 * 24 * 60 * 60;
    if ( !fUseFilter ) {
        nInterval       = nOriginalInterval;
        nTargetTimespan = nOriginalTargetTimespan;
    }

    // Only change once per interval
    if (  (fUseFilter && (pindexLast->nHeight+1) % nInterval != 0) ||
         (!fUseFilter && (pindexLast->nHeight+1) % 2016 != 0))
    {
        // Special difficulty rule for testnet:
        if (fTestNet)
        {
            // If the new block's timestamp is more than 2* 10 minutes
            // then allow mining of a min-difficulty block.
            if (pblock->nTime > pindexLast->nTime + nTargetSpacing*2)
                return nProofOfWorkLimit;
            else
            {
                // Return the last non-special-min-difficulty-rules-block
                const CBlockIndex* pindex = pindexLast;
                while (pindex->pprev && pindex->nHeight % nInterval != 0 && pindex->nBits == nProofOfWorkLimit)
                    pindex = pindex->pprev;
                return pindex->nBits;
            }
        }

        return pindexLast->nBits;
    }

    mpq dAdjustmentFactor;

    if ( fUseFilter ) {
        int32_t vTimeDelta[WINDOW];

        size_t idx = 0;
        const CBlockIndex *pitr = pindexLast;
        for ( ; idx!=WINDOW && pitr && pitr->pprev; ++idx, pitr=pitr->pprev )
            vTimeDelta[idx] = (int32_t)(pitr->GetBlockTime() - pitr->pprev->GetBlockTime());
        for ( ; idx!=WINDOW; ++idx )
            vTimeDelta[idx] = (int32_t)nTargetSpacing;

        int64_t vFilteredTime = 0;
        for ( idx=0; idx<WINDOW; ++idx )
            vFilteredTime += (int64_t)kFilterCoeff[idx] * (int64_t)vTimeDelta[idx];
        mpq dFilteredInterval = i64_to_mpq(vFilteredTime) / kTwoToTheThirtyOne;

        dAdjustmentFactor = kOne - kGain * (dFilteredInterval - kTargetInterval) / kTargetInterval;
        if ( dAdjustmentFactor > kLimiterUp )
            dAdjustmentFactor = kLimiterUp;
        else if ( dAdjustmentFactor < kLimiterDown )
            dAdjustmentFactor = kLimiterDown;
    } else {
        // This fixes an issue where a 51% attack can change difficulty at will.
        // Go back the full period unless it's the first retarget after genesis.
        // Code courtesy of Art Forz
        int blockstogoback = nInterval-1;
        if ((pindexLast->nHeight+1) != nInterval)
            blockstogoback = nInterval;

        // Go back by what we want to be 14 days worth of blocks
        const CBlockIndex* pindexFirst = pindexLast;
        for (int i = 0; pindexFirst && i < blockstogoback; i++)
            pindexFirst = pindexFirst->pprev;
        assert(pindexFirst);

        // Limit adjustment step
        int64 nActualTimespan = pindexLast->GetBlockTime() - pindexFirst->GetBlockTime();
        printf("  nActualTimespan = %"PRI64d"  before bounds\n", nActualTimespan);
        if (nActualTimespan < nTargetTimespan/4)
            nActualTimespan = nTargetTimespan/4;
        if (nActualTimespan > nTargetTimespan*4)
            nActualTimespan = nTargetTimespan*4;

        dAdjustmentFactor = i64_to_mpq(nTargetTimespan) /
                            i64_to_mpq(nActualTimespan);
    }

    // Retarget
    CBigNum bnNew;
    bnNew.SetCompact(pindexLast->nBits);
    bnNew *= mpz_to_i64(dAdjustmentFactor.get_den());
    bnNew /= mpz_to_i64(dAdjustmentFactor.get_num());

    if (bnNew > bnProofOfWorkLimit)
        bnNew = bnProofOfWorkLimit;

    /// debug print
    printf("GetNextWorkRequired RETARGET\n");
    printf("dAdjustmentFactor = %g\n", dAdjustmentFactor.get_d());
    printf("Before: %08x  %s\n", pindexLast->nBits, CBigNum().SetCompact(pindexLast->nBits).getuint256().ToString().c_str());
    printf("After:  %08x  %s\n", bnNew.GetCompact(), bnNew.getuint256().ToString().c_str());

    return bnNew.GetCompact();
}

bool CheckProofOfWork(uint256 hash, unsigned int nBits)
{
    CBigNum bnTarget;
    bnTarget.SetCompact(nBits);

    // Check range
    if (bnTarget <= 0 || bnTarget > bnProofOfWorkLimit)
        return error("CheckProofOfWork() : nBits below minimum work");

    // Check proof of work matches claimed amount
    if (hash > bnTarget.getuint256())
        return error("CheckProofOfWork() : hash doesn't match nBits");

    return true;
}

// Return maximum amount of blocks that other nodes claim to have
int GetNumBlocksOfPeers()
{
    return std::max(cPeerBlockCounts.median(), Checkpoints::GetTotalBlocksEstimate());
}

bool IsInitialBlockDownload()
{
    if (pindexBest == NULL || nBestHeight < Checkpoints::GetTotalBlocksEstimate())
        return true;
    static int64 nLastUpdate;
    static CBlockIndex* pindexLastBest;
    if (pindexBest != pindexLastBest)
    {
        pindexLastBest = pindexBest;
        nLastUpdate = GetTime();
    }
    return (GetTime() - nLastUpdate < 10 &&
            pindexBest->GetBlockTime() < GetTime() - 24 * 60 * 60);
}

void static InvalidChainFound(CBlockIndex* pindexNew)
{
    if (pindexNew->bnChainWork > bnBestInvalidWork)
    {
        bnBestInvalidWork = pindexNew->bnChainWork;
        CTxDB().WriteBestInvalidWork(bnBestInvalidWork);
        uiInterface.NotifyBlocksChanged();
    }
    printf("InvalidChainFound: invalid block=%s  height=%d  work=%s  date=%s\n",
      pindexNew->GetBlockHash().ToString().substr(0,20).c_str(), pindexNew->nHeight,
      pindexNew->bnChainWork.ToString().c_str(), DateTimeStrFormat("%x %H:%M:%S",
      pindexNew->GetBlockTime()).c_str());
    printf("InvalidChainFound:  current best=%s  height=%d  work=%s  date=%s\n",
      hashBestChain.ToString().substr(0,20).c_str(), nBestHeight, bnBestChainWork.ToString().c_str(),
      DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()).c_str());
    if (pindexBest && bnBestInvalidWork > bnBestChainWork + pindexBest->GetBlockWork() * 6)
        printf("InvalidChainFound: Warning: Displayed transactions may not be correct! You may need to upgrade, or other nodes may need to upgrade.\n");
}

void CBlock::UpdateTime(const CBlockIndex* pindexPrev)
{
    nTime = max(pindexPrev->GetMedianTimePast()+1, GetAdjustedTime());

    // Updating time can change work required on testnet:
    if (fTestNet)
        nBits = GetNextWorkRequired(pindexPrev, this);
}











bool CTransaction::DisconnectInputs(CTxDB& txdb)
{
    // Relinquish previous transactions' spent pointers
    if (!IsCoinBase())
    {
        BOOST_FOREACH(const CTxIn& txin, vin)
        {
            COutPoint prevout = txin.prevout;

            // Get prev txindex from disk
            CTxIndex txindex;
            if (!txdb.ReadTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : ReadTxIndex failed");

            if (prevout.n >= txindex.vSpent.size())
                return error("DisconnectInputs() : prevout.n out of range");

            // Mark outpoint as not spent
            txindex.vSpent[prevout.n].SetNull();

            // Write back
            if (!txdb.UpdateTxIndex(prevout.hash, txindex))
                return error("DisconnectInputs() : UpdateTxIndex failed");
        }
    }

    // Remove transaction from index
    // This can fail if a duplicate of this transaction was in a chain that got
    // reorganized away. This is only possible if this transaction was completely
    // spent, so erasing it would be a no-op anyway.
    txdb.EraseTxIndex(*this);

    return true;
}


bool CTransaction::FetchInputs(CTxDB& txdb, const map<uint256, CTxIndex>& mapTestPool,
                               bool fBlock, bool fMiner, MapPrevTx& inputsRet, bool& fInvalid) const
{
    // FetchInputs can return false either because we just haven't seen some inputs
    // (in which case the transaction should be stored as an orphan)
    // or because the transaction is malformed (in which case the transaction should
    // be dropped).  If tx is definitely invalid, fInvalid will be set to true.
    fInvalid = false;

    if (IsCoinBase())
        return true; // Coinbase transactions have no inputs to fetch.

    for (unsigned int i = 0; i < vin.size(); i++)
    {
        COutPoint prevout = vin[i].prevout;
        if (inputsRet.count(prevout.hash))
            continue; // Got it already

        // Read txindex
        CTxIndex& txindex = inputsRet[prevout.hash].first;
        bool fFound = true;
        if ((fBlock || fMiner) && mapTestPool.count(prevout.hash))
        {
            // Get txindex from current proposed changes
            txindex = mapTestPool.find(prevout.hash)->second;
        }
        else
        {
            // Read txindex from txdb
            fFound = txdb.ReadTxIndex(prevout.hash, txindex);
        }
        if (!fFound && (fBlock || fMiner))
            return fMiner ? false : error("FetchInputs() : %s prev tx %s index entry not found", GetHash().ToString().substr(0,10).c_str(),  prevout.hash.ToString().substr(0,10).c_str());

        // Read txPrev
        CTransaction& txPrev = inputsRet[prevout.hash].second;
        if (!fFound || txindex.pos == CDiskTxPos(1,1,1))
        {
            // Get prev tx from single transactions in memory
            {
                LOCK(mempool.cs);
                if (!mempool.exists(prevout.hash))
                    return error("FetchInputs() : %s mempool Tx prev not found %s", GetHash().ToString().substr(0,10).c_str(),  prevout.hash.ToString().substr(0,10).c_str());
                txPrev = mempool.lookup(prevout.hash);
            }
            if (!fFound)
                txindex.vSpent.resize(txPrev.vout.size());
        }
        else
        {
            // Get prev tx from disk
            if (!txPrev.ReadFromDisk(txindex.pos))
                return error("FetchInputs() : %s ReadFromDisk prev tx %s failed", GetHash().ToString().substr(0,10).c_str(),  prevout.hash.ToString().substr(0,10).c_str());
        }
    }

    // Make sure all prevout.n indexes are valid:
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const COutPoint prevout = vin[i].prevout;
        assert(inputsRet.count(prevout.hash) != 0);
        const CTxIndex& txindex = inputsRet[prevout.hash].first;
        const CTransaction& txPrev = inputsRet[prevout.hash].second;
        if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
        {
            // Revisit this if/when transaction replacement is implemented and allows
            // adding inputs:
            fInvalid = true;
            return DoS(100, error("FetchInputs() : %s prevout.n out of range %d %"PRIszu" %"PRIszu" prev tx %s\n%s", GetHash().ToString().substr(0,10).c_str(), prevout.n, txPrev.vout.size(), txindex.vSpent.size(), prevout.hash.ToString().substr(0,10).c_str(), txPrev.ToString().c_str()));
        }
    }

    return true;
}

const CTxOut& CTransaction::GetOutputFor(const CTxIn& input, const MapPrevTx& inputs) const
{
    MapPrevTx::const_iterator mi = inputs.find(input.prevout.hash);
    if (mi == inputs.end())
        throw std::runtime_error("CTransaction::GetOutputFor() : prevout.hash not found");

    const CTransaction& txPrev = (mi->second).second;
    if (input.prevout.n >= txPrev.vout.size())
        throw std::runtime_error("CTransaction::GetOutputFor() : prevout.n out of range");

    return txPrev.vout[input.prevout.n];
}

mpq GetTimeAdjustedValue(int64 nInitialValue, int nRelativeDepth)
{
    return GetTimeAdjustedValue(i64_to_mpq(nInitialValue), nRelativeDepth);
}

mpq GetTimeAdjustedValue(const mpz &zInitialValue, int nRelativeDepth)
{
    mpq initial_value(zInitialValue);
    return GetTimeAdjustedValue(initial_value, nRelativeDepth);
}

mpq GetTimeAdjustedValue(const mpq& qInitialValue, int nRelativeDepth)
{
    if ( 0 == nRelativeDepth )
        return qInitialValue;

    mpfr_t rate, mp;
    mpfr_inits2(113, rate, mp, (mpfr_ptr) 0);
    mpfr_set_ui(mp,       DEMURRAGE_RATE-1, GMP_RNDN);
    mpfr_div_ui(rate, mp, DEMURRAGE_RATE,   GMP_RNDN);
    mpfr_pow_si(mp, rate, nRelativeDepth,   GMP_RNDN);

    int exponent;
    mpz_t numerator, denominator;
    mpz_init(numerator);
    mpz_init(denominator);
    exponent = mpfr_get_z_exp(numerator, mp);
    mpz_set_ui(denominator, 1);
    if ( exponent >= 0 )
        mpz_mul_2exp(numerator, numerator, exponent);
    else
        mpz_mul_2exp(denominator, denominator, -exponent);

    mpfr_clears(rate, mp, (mpfr_ptr) 0);

    mpq adjustment;
    mpz_set(adjustment.get_num_mpz_t(), numerator);
    mpz_set(adjustment.get_den_mpz_t(), denominator);
    adjustment.canonicalize();

    mpz_clear(numerator);
    mpz_clear(denominator);

    return adjustment * qInitialValue;
}

mpq GetPresentValue(const CTransaction& tx, const CTxOut& output, int nBlockHeight)
{
    return GetTimeAdjustedValue(output.nValue, nBlockHeight-tx.nRefHeight);
}

mpq CTransaction::GetValueIn(const MapPrevTx& inputs) const
{
    if (IsCoinBase())
        return 0;

    mpq nResult = 0, nInput;
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        MapPrevTx::const_iterator mi = inputs.find(vin[i].prevout.hash);
        if (mi == inputs.end())
            throw std::runtime_error("CTransaction::GetValueIn() : prevout.hash not found");

        const CTransaction& txPrev = (mi->second).second;
        if (vin[i].prevout.n >= txPrev.vout.size())
            throw std::runtime_error("CTransaction::GetValueIn() : prevout.n out of range");

        const CTxOut& txOut = txPrev.vout[vin[i].prevout.n];
        nInput = GetPresentValue(txPrev, txOut, nRefHeight);
        nResult += nInput;
        // Check for negative or overflow input values
        if (!MoneyRange(nInput) || !MoneyRange(nResult))
            throw DoS(100, error("CTransaction::GetValueIn() : txin values out of range"));
    }
    return nResult;

}

unsigned int CTransaction::GetP2SHSigOpCount(const MapPrevTx& inputs) const
{
    if (IsCoinBase())
        return 0;

    unsigned int nSigOps = 0;
    for (unsigned int i = 0; i < vin.size(); i++)
    {
        const CTxOut& prevout = GetOutputFor(vin[i], inputs);
        if (prevout.scriptPubKey.IsPayToScriptHash())
            nSigOps += prevout.scriptPubKey.GetSigOpCount(vin[i].scriptSig);
    }
    return nSigOps;
}

bool CTransaction::ConnectInputs(MapPrevTx inputs,
                                 map<uint256, CTxIndex>& mapTestPool, const CDiskTxPos& posThisTx,
                                 const CBlockIndex* pindexBlock, bool fBlock, bool fMiner, bool fStrictPayToScriptHash)
{
    // Take over previous transactions' spent pointers
    // fBlock is true when this is called from AcceptBlock when a new best-block is added to the blockchain
    // fMiner is true when called from the internal x miner
    // ... both are false when called from CTransaction::AcceptToMemoryPool
    if (!IsCoinBase())
    {
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            COutPoint prevout = vin[i].prevout;
            assert(inputs.count(prevout.hash) > 0);
            CTxIndex& txindex = inputs[prevout.hash].first;
            CTransaction& txPrev = inputs[prevout.hash].second;

            if (prevout.n >= txPrev.vout.size() || prevout.n >= txindex.vSpent.size())
                return DoS(100, error("ConnectInputs() : %s prevout.n out of range %d %"PRIszu" %"PRIszu" prev tx %s\n%s", GetHash().ToString().substr(0,10).c_str(), prevout.n, txPrev.vout.size(), txindex.vSpent.size(), prevout.hash.ToString().substr(0,10).c_str(), txPrev.ToString().c_str()));

            // If prev is coinbase, check that it's matured
            if (txPrev.IsCoinBase())
                for (const CBlockIndex* pindex = pindexBlock; pindex && pindexBlock->nHeight - pindex->nHeight < COINBASE_MATURITY; pindex = pindex->pprev)
                    if (pindex->nBlockPos == txindex.pos.nBlockPos && pindex->nFile == txindex.pos.nFile)
                        return error("ConnectInputs() : tried to spend coinbase at depth %d", pindexBlock->nHeight - pindex->nHeight);

        }
        mpq nValueIn = GetValueIn(inputs);
        if ( ! MoneyRange(nValueIn) )
            return DoS(100, error("ConnectInputs() : txin values out of range"));
        if ( GetValueOut() > nValueIn )
            return DoS(100, error("ConnectInputs() : txout larger than txin"));
        // The first loop above does all the inexpensive checks.
        // Only if ALL inputs pass do we perform expensive ECDSA signature checks.
        // Helps prevent CPU exhaustion attacks.
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            COutPoint prevout = vin[i].prevout;
            assert(inputs.count(prevout.hash) > 0);
            CTxIndex& txindex = inputs[prevout.hash].first;
            CTransaction& txPrev = inputs[prevout.hash].second;

            if ( txPrev.nRefHeight > nRefHeight )
                return DoS(100, error("ConnectInputs() : input height less than output height"));

            // Check for conflicts (double-spend)
            // This doesn't trigger the DoS code on purpose; if it did, it would make it easier
            // for an attacker to attempt to split the network.
            if (!txindex.vSpent[prevout.n].IsNull())
                return fMiner ? false : error("ConnectInputs() : %s prev tx already used at %s", GetHash().ToString().substr(0,10).c_str(), txindex.vSpent[prevout.n].ToString().c_str());

            // Skip ECDSA signature verification when connecting blocks (fBlock=true)
            // before the last blockchain checkpoint. This is safe because block merkle hashes are
            // still computed and checked, and any change will be caught at the next checkpoint.
            if (!(fBlock && (nBestHeight < Checkpoints::GetTotalBlocksEstimate())))
            {
                // Verify signature
                if (!VerifySignature(txPrev, *this, i, fStrictPayToScriptHash, 0))
                {
                    // only during transition phase for P2SH: do not invoke anti-DoS code for
                    // potentially old clients relaying bad P2SH transactions
                    if (fStrictPayToScriptHash && VerifySignature(txPrev, *this, i, false, 0))
                        return error("ConnectInputs() : %s P2SH VerifySignature failed", GetHash().ToString().substr(0,10).c_str());

                    return DoS(100,error("ConnectInputs() : %s VerifySignature failed", GetHash().ToString().substr(0,10).c_str()));
                }
            }

            // Mark outpoints as spent
            txindex.vSpent[prevout.n] = posThisTx;

            // Write back
            if (fBlock || fMiner)
            {
                mapTestPool[prevout.hash] = txindex;
            }
        }

    }

    return true;
}


bool CTransaction::ClientConnectInputs(CTxDB& txdb)
{
    if (IsCoinBase())
        return false;

    // Take over previous transactions' spent pointers
    {
        LOCK(mempool.cs);
        for (unsigned int i = 0; i < vin.size(); i++)
        {
            // Get prev tx from single transactions in memory
            COutPoint prevout = vin[i].prevout;
            if (!mempool.exists(prevout.hash))
                return false;
            CTransaction& txPrev = mempool.lookup(prevout.hash);

            if (prevout.n >= txPrev.vout.size())
                return false;

            // Verify signature
            if (!VerifySignature(txPrev, *this, i, true, 0))
                return error("ConnectInputs() : VerifySignature failed");

            ///// this is redundant with the mempool.mapNextTx stuff,
            ///// not sure which I want to get rid of
            ///// this has to go away now that posNext is gone
            // // Check for conflicts
            // if (!txPrev.vout[prevout.n].posNext.IsNull())
            //     return error("ConnectInputs() : prev tx already used");
            //
            // // Flag outpoints as used
            // txPrev.vout[prevout.n].posNext = posThisTx;
        }

        MapPrevTx mapInputs;
        map<uint256, CTxIndex> mapUnused;
        bool fInvalid = false;
        if (!FetchInputs(txdb, mapUnused, false, false, mapInputs, fInvalid))
        {
            if (fInvalid)
                return error("CTransaction::ClientConnectInputs() : FetchInputs found invalid tx");
            return false;
        }

        for (unsigned int i = 0; i < vin.size(); i++)
        {
            CTransaction& txPrev = mapInputs[vin[i].prevout.hash].second;
            if ( txPrev.nRefHeight > nRefHeight )
                return DoS(100, error("ConnectInputs() : input height less than output height"));
        }

        mpq nValueIn = GetValueIn(mapInputs);
        if ( ! MoneyRange(nValueIn) )
            return error("ClientConnectInputs() : txin values out of range");
        if ( GetValueOut() > nValueIn )
            return error("ClientConnectInputs() : value out larger than value in");
    }

    return true;
}




bool CBlock::DisconnectBlock(CTxDB& txdb, CBlockIndex* pindex)
{
    // Disconnect in reverse order
    for (int i = vtx.size()-1; i >= 0; i--)
        if (!vtx[i].DisconnectInputs(txdb))
            return false;

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = 0;
        if (!txdb.WriteBlockIndex(blockindexPrev))
            return error("DisconnectBlock() : WriteBlockIndex failed");
    }

    return true;
}

bool CBlock::ConnectBlock(CTxDB& txdb, CBlockIndex* pindex, bool fJustCheck)
{
    // Check it again in case a previous version let a bad block in
    if (!CheckBlock(!fJustCheck, !fJustCheck))
        return false;

    // Do not allow blocks that contain transactions which 'overwrite' older transactions,
    // unless those are already completely spent.
    // If such overwrites are allowed, coinbases and transactions depending upon those
    // can be duplicated to remove the ability to spend the first instance -- even after
    // being sent to another address.
    // See BIP30 and http://r6.ca/blog/20120206T005236Z.html for more information.
    // This logic is not necessary for memory pool transactions, as AcceptToMemoryPool
    // already refuses previously-known transaction ids entirely.
    // This rule was originally applied all blocks whose timestamp was after March 15, 2012, 0:00 UTC.
    // Now that the whole chain is irreversibly beyond that time it is applied to all blocks except the
    // two in the chain that violate it. This prevents exploiting the issue against nodes in their
    // initial block download.
    bool fEnforceBIP30 = (!pindex->phashBlock) || // Enforce on CreateNewBlock invocations which don't have a hash.
                          !((pindex->nHeight==91842 && pindex->GetBlockHash() == uint256("0x00000000000a4d0a398161ffc163c503763b1f4360639393e0e4c8e300e0caec")) ||
                           (pindex->nHeight==91880 && pindex->GetBlockHash() == uint256("0x00000000000743f190a18c5577a3c2d2a1f610ae9601ac046a38084ccb7cd721")));

    // BIP16 didn't become active until Apr 1 2012
    int64 nBIP16SwitchTime = 1333238400;
    bool fStrictPayToScriptHash = (pindex->nTime >= nBIP16SwitchTime);

    //// issue here: it doesn't know the version
    unsigned int nTxPos;
    if (fJustCheck)
        // FetchInputs treats CDiskTxPos(1,1,1) as a special "refer to memorypool" indicator
        // Since we're just checking the block and not actually connecting it, it might not (and probably shouldn't) be on the disk to get the transaction from
        nTxPos = 1;
    else
        nTxPos = pindex->nBlockPos + ::GetSerializeSize(CBlock(), SER_DISK, CLIENT_VERSION) - 1 + GetSizeOfCompactSize(vtx.size());

    map<uint256, CTxIndex> mapQueuedChanges;
    mpq nFees = 0;
    unsigned int nSigOps = 0;
    BOOST_FOREACH(CTransaction& tx, vtx)
    {
        uint256 hashTx = tx.GetHash();

        if (fEnforceBIP30) {
            CTxIndex txindexOld;
            if (txdb.ReadTxIndex(hashTx, txindexOld)) {
                BOOST_FOREACH(CDiskTxPos &pos, txindexOld.vSpent)
                    if (pos.IsNull())
                        return false;
            }
        }

        nSigOps += tx.GetLegacySigOpCount();
        if (nSigOps > MAX_BLOCK_SIGOPS)
            return DoS(100, error("ConnectBlock() : too many sigops"));

        CDiskTxPos posThisTx(pindex->nFile, pindex->nBlockPos, nTxPos);
        if (!fJustCheck)
            nTxPos += ::GetSerializeSize(tx, SER_DISK, CLIENT_VERSION);

        MapPrevTx mapInputs;
        if (!tx.IsCoinBase())
        {
            bool fInvalid;
            if (!tx.FetchInputs(txdb, mapQueuedChanges, true, false, mapInputs, fInvalid))
                return error("ConnectBlock() : unable to fetch inputs for transaction %s", tx.GetHash().ToString().c_str());

            if (fStrictPayToScriptHash)
            {
                // Add in sigops done by pay-to-script-hash inputs;
                // this is to prevent a "rogue miner" from creating
                // an incredibly-expensive-to-validate block.
                nSigOps += tx.GetP2SHSigOpCount(mapInputs);
                if (nSigOps > MAX_BLOCK_SIGOPS)
                    return DoS(100, error("ConnectBlock() : too many sigops"));
            }

            if ( tx.nRefHeight > pindex->nHeight )
                return DoS(100, error("ConnectBlock() : tx height > block height"));

            mpq qNet = tx.GetValueIn(mapInputs) - tx.GetValueOut();
            nFees += GetTimeAdjustedValue(qNet, pindex->nHeight - tx.nRefHeight);

            if (!tx.ConnectInputs(mapInputs, mapQueuedChanges, posThisTx, pindex, true, false, fStrictPayToScriptHash))
                return error("ConnectBlock() : unable to connect inputs");
        }

        mapQueuedChanges[hashTx] = CTxIndex(posThisTx, tx.vout.size());
    }

    if ( vtx[0].nRefHeight != pindex->nHeight )
        return DoS(100, error("ConnectBlock() : coinbase height != block height"));

    mpq qActualCoinbaseValue = GetTimeAdjustedValue(vtx[0].GetValueOut(), pindex->nHeight - vtx[0].nRefHeight);
    mpq qAllowedCoinbaseValue = GetBlockValue(pindex->nHeight, nFees);
    if ( qActualCoinbaseValue > qAllowedCoinbaseValue )
        return error("ConnectBlock() : coinbase pays too much (actual=%s vs limit=%s)", FormatMoney(qActualCoinbaseValue).c_str(), FormatMoney(qAllowedCoinbaseValue).c_str());

    std::map<CTxDestination, mpq> mapBudget;

    mpq nIDAmount = GetInitialDistributionAmount(pindex->nHeight);
    CBudget budgetID = GetInitialDistributionBudget(pindex->nHeight);
    ApplyBudget(nIDAmount, budgetID, mapBudget);

    mpq nPSAmount = GetPerpetualSubsidyAmount(pindex->nHeight);
    CBudget budgetPS = GetPerpetualSubsidyBudget(pindex->nHeight);
    ApplyBudget(nPSAmount, budgetPS, mapBudget);

    CBudget budgetTF = GetTransactionFeeBudget(pindex->nHeight);
    ApplyBudget(nFees, budgetTF, mapBudget);

    if (!VerifyBudget(mapBudget, vtx, pindex->nHeight))
        return error("ConnectBlock() : block does not meet budget requirements");

    if (fJustCheck)
        return true;

    // Write queued txindex changes
    for (map<uint256, CTxIndex>::iterator mi = mapQueuedChanges.begin(); mi != mapQueuedChanges.end(); ++mi)
    {
        if (!txdb.UpdateTxIndex((*mi).first, (*mi).second))
            return error("ConnectBlock() : UpdateTxIndex failed");
    }

    // Update block index on disk without changing it in memory.
    // The memory index structure will be changed after the db commits.
    if (pindex->pprev)
    {
        CDiskBlockIndex blockindexPrev(pindex->pprev);
        blockindexPrev.hashNext = pindex->GetBlockHash();
        if (!txdb.WriteBlockIndex(blockindexPrev))
            return error("ConnectBlock() : WriteBlockIndex failed");
    }

    // Watch for transactions paying to me
    BOOST_FOREACH(CTransaction& tx, vtx)
        SyncWithWallets(tx, this, true);

    return true;
}

bool static Reorganize(CTxDB& txdb, CBlockIndex* pindexNew)
{
    printf("REORGANIZE\n");

    // Find the fork
    CBlockIndex* pfork = pindexBest;
    CBlockIndex* plonger = pindexNew;
    while (pfork != plonger)
    {
        while (plonger->nHeight > pfork->nHeight)
            if (!(plonger = plonger->pprev))
                return error("Reorganize() : plonger->pprev is null");
        if (pfork == plonger)
            break;
        if (!(pfork = pfork->pprev))
            return error("Reorganize() : pfork->pprev is null");
    }

    // List of what to disconnect
    vector<CBlockIndex*> vDisconnect;
    for (CBlockIndex* pindex = pindexBest; pindex != pfork; pindex = pindex->pprev)
        vDisconnect.push_back(pindex);

    // List of what to connect
    vector<CBlockIndex*> vConnect;
    for (CBlockIndex* pindex = pindexNew; pindex != pfork; pindex = pindex->pprev)
        vConnect.push_back(pindex);
    reverse(vConnect.begin(), vConnect.end());

    printf("REORGANIZE: Disconnect %"PRIszu" blocks; %s..%s\n", vDisconnect.size(), pfork->GetBlockHash().ToString().substr(0,20).c_str(), pindexBest->GetBlockHash().ToString().substr(0,20).c_str());
    printf("REORGANIZE: Connect %"PRIszu" blocks; %s..%s\n", vConnect.size(), pfork->GetBlockHash().ToString().substr(0,20).c_str(), pindexNew->GetBlockHash().ToString().substr(0,20).c_str());

    // Disconnect shorter branch
    vector<CTransaction> vResurrect;
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
    {
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("Reorganize() : ReadFromDisk for disconnect failed");
        if (!block.DisconnectBlock(txdb, pindex))
            return error("Reorganize() : DisconnectBlock %s failed", pindex->GetBlockHash().ToString().substr(0,20).c_str());

        // Queue memory transactions to resurrect
        BOOST_FOREACH(const CTransaction& tx, block.vtx)
            if (!tx.IsCoinBase())
                vResurrect.push_back(tx);
    }

    // Connect longer branch
    vector<CTransaction> vDelete;
    for (unsigned int i = 0; i < vConnect.size(); i++)
    {
        CBlockIndex* pindex = vConnect[i];
        CBlock block;
        if (!block.ReadFromDisk(pindex))
            return error("Reorganize() : ReadFromDisk for connect failed");
        if (!block.ConnectBlock(txdb, pindex))
        {
            // Invalid block
            return error("Reorganize() : ConnectBlock %s failed", pindex->GetBlockHash().ToString().substr(0,20).c_str());
        }

        // Queue memory transactions to delete
        BOOST_FOREACH(const CTransaction& tx, block.vtx)
            vDelete.push_back(tx);
    }
    if (!txdb.WriteHashBestChain(pindexNew->GetBlockHash()))
        return error("Reorganize() : WriteHashBestChain failed");

    // Make sure it's successfully written to disk before changing memory structure
    if (!txdb.TxnCommit())
        return error("Reorganize() : TxnCommit failed");

    // Disconnect shorter branch
    BOOST_FOREACH(CBlockIndex* pindex, vDisconnect)
        if (pindex->pprev)
            pindex->pprev->pnext = NULL;

    // Connect longer branch
    BOOST_FOREACH(CBlockIndex* pindex, vConnect)
        if (pindex->pprev)
            pindex->pprev->pnext = pindex;

    // Resurrect memory transactions that were in the disconnected branch
    BOOST_FOREACH(CTransaction& tx, vResurrect)
        tx.AcceptToMemoryPool(txdb, false);

    // Delete redundant memory transactions that are in the connected branch
    BOOST_FOREACH(CTransaction& tx, vDelete)
        mempool.remove(tx);

    printf("REORGANIZE: done\n");

    return true;
}


// Called from inside SetBestChain: attaches a block to the new best chain being built
bool CBlock::SetBestChainInner(CTxDB& txdb, CBlockIndex *pindexNew)
{
    uint256 hash = GetHash();

    // Adding to current best branch
    if (!ConnectBlock(txdb, pindexNew) || !txdb.WriteHashBestChain(hash))
    {
        txdb.TxnAbort();
        InvalidChainFound(pindexNew);
        return false;
    }
    if (!txdb.TxnCommit())
        return error("SetBestChain() : TxnCommit failed");

    // Add to current best branch
    pindexNew->pprev->pnext = pindexNew;

    // Delete redundant memory transactions
    BOOST_FOREACH(CTransaction& tx, vtx)
        mempool.remove(tx);

    return true;
}

bool CBlock::SetBestChain(CTxDB& txdb, CBlockIndex* pindexNew)
{
    uint256 hash = GetHash();

    if (!txdb.TxnBegin())
        return error("SetBestChain() : TxnBegin failed");

    if (pindexGenesisBlock == NULL && hash == hashGenesisBlock)
    {
        txdb.WriteHashBestChain(hash);
        if (!txdb.TxnCommit())
            return error("SetBestChain() : TxnCommit failed");
        pindexGenesisBlock = pindexNew;
    }
    else if (hashPrevBlock == hashBestChain)
    {
        if (!SetBestChainInner(txdb, pindexNew))
            return error("SetBestChain() : SetBestChainInner failed");
    }
    else
    {
        // the first block in the new chain that will cause it to become the new best chain
        CBlockIndex *pindexIntermediate = pindexNew;

        // list of blocks that need to be connected afterwards
        std::vector<CBlockIndex*> vpindexSecondary;

        // Reorganize is costly in terms of db load, as it works in a single db transaction.
        // Try to limit how much needs to be done inside
        while (pindexIntermediate->pprev && pindexIntermediate->pprev->bnChainWork > pindexBest->bnChainWork)
        {
            vpindexSecondary.push_back(pindexIntermediate);
            pindexIntermediate = pindexIntermediate->pprev;
        }

        if (!vpindexSecondary.empty())
            printf("Postponing %"PRIszu" reconnects\n", vpindexSecondary.size());

        // Switch to new best branch
        if (!Reorganize(txdb, pindexIntermediate))
        {
            txdb.TxnAbort();
            InvalidChainFound(pindexNew);
            return error("SetBestChain() : Reorganize failed");
        }

        // Connect further blocks
        BOOST_REVERSE_FOREACH(CBlockIndex *pindex, vpindexSecondary)
        {
            CBlock block;
            if (!block.ReadFromDisk(pindex))
            {
                printf("SetBestChain() : ReadFromDisk failed\n");
                break;
            }
            if (!txdb.TxnBegin()) {
                printf("SetBestChain() : TxnBegin 2 failed\n");
                break;
            }
            // errors now are not fatal, we still did a reorganisation to a new chain in a valid way
            if (!block.SetBestChainInner(txdb, pindex))
                break;
        }
    }

    // Update best block in wallet (so we can detect restored wallets)
    bool fIsInitialDownload = IsInitialBlockDownload();
    if (!fIsInitialDownload)
    {
        const CBlockLocator locator(pindexNew);
        ::SetBestChain(locator);
    }

    // New best block
    hashBestChain = hash;
    pindexBest = pindexNew;
    pblockindexFBBHLast = NULL;
    nBestHeight = pindexBest->nHeight;
    bnBestChainWork = pindexNew->bnChainWork;
    nTimeBestReceived = GetTime();
    nTransactionsUpdated++;
    printf("SetBestChain: new best=%s  height=%d  work=%s  date=%s\n",
      hashBestChain.ToString().substr(0,20).c_str(), nBestHeight, bnBestChainWork.ToString().c_str(),
      DateTimeStrFormat("%x %H:%M:%S", pindexBest->GetBlockTime()).c_str());

    // Check the version of the last 100 blocks to see if we need to upgrade:
    if (!fIsInitialDownload)
    {
        int nUpgraded = 0;
        const CBlockIndex* pindex = pindexBest;
        for (int i = 0; i < 100 && pindex != NULL; i++)
        {
            if (pindex->nVersion > CBlock::CURRENT_VERSION)
                ++nUpgraded;
            pindex = pindex->pprev;
        }
        if (nUpgraded > 0)
            printf("SetBestChain: %d of last 100 blocks above version %d\n", nUpgraded, CBlock::CURRENT_VERSION);
        if (nUpgraded > 100/2)
            // strMiscWarning is read by GetWarnings(), called by Qt and the JSON-RPC code to warn the user:
            strMiscWarning = _("Warning: This version is obsolete, upgrade required!");
    }

    std::string strCmd = GetArg("-blocknotify", "");

    if (!fIsInitialDownload && !strCmd.empty())
    {
        boost::replace_all(strCmd, "%s", hashBestChain.GetHex());
        boost::thread t(runCommand, strCmd); // thread runs free
    }

    return true;
}


bool CBlock::AddToBlockIndex(unsigned int nFile, unsigned int nBlockPos)
{
    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return error("AddToBlockIndex() : %s already exists", hash.ToString().substr(0,20).c_str());

    // Construct new block index object
    CBlockIndex* pindexNew = new CBlockIndex(nFile, nBlockPos, *this);
    if (!pindexNew)
        return error("AddToBlockIndex() : new CBlockIndex failed");
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.insert(make_pair(hash, pindexNew)).first;
    pindexNew->phashBlock = &((*mi).first);
    map<uint256, CBlockIndex*>::iterator miPrev = mapBlockIndex.find(hashPrevBlock);
    if (miPrev != mapBlockIndex.end())
    {
        pindexNew->pprev = (*miPrev).second;
        pindexNew->nHeight = pindexNew->pprev->nHeight + 1;
    }
    pindexNew->bnChainWork = (pindexNew->pprev ? pindexNew->pprev->bnChainWork : 0) + pindexNew->GetBlockWork();

    CTxDB txdb;
    if (!txdb.TxnBegin())
        return false;
    txdb.WriteBlockIndex(CDiskBlockIndex(pindexNew));
    if (!txdb.TxnCommit())
        return false;

    // New best
    if (pindexNew->bnChainWork > bnBestChainWork)
        if (!SetBestChain(txdb, pindexNew))
            return false;

    txdb.Close();

    if (pindexNew == pindexBest)
    {
        // Notify UI to display prev block's coinbase if it was ours
        static uint256 hashPrevBestCoinBase;
        UpdatedTransaction(hashPrevBestCoinBase);
        hashPrevBestCoinBase = vtx[0].GetHash();
    }

    uiInterface.NotifyBlocksChanged();
    return true;
}




bool CBlock::CheckBlock(bool fCheckPOW, bool fCheckMerkleRoot) const
{
    // These are checks that are independent of context
    // that can be verified before saving an orphan block.

    // Size limits
    if (vtx.empty() || vtx.size() > MAX_BLOCK_SIZE || ::GetSerializeSize(*this, SER_NETWORK, PROTOCOL_VERSION) > MAX_BLOCK_SIZE)
        return DoS(100, error("CheckBlock() : size limits failed"));

    // Check proof of work matches claimed amount
    if (fCheckPOW && !CheckProofOfWork(GetHash(), nBits))
        return DoS(50, error("CheckBlock() : proof of work failed"));

    // Check timestamp
    if (GetBlockTime() > GetAdjustedTime() + 2 * 60 * 60)
        return error("CheckBlock() : block timestamp too far in the future");

    // First transaction must be coinbase, the rest must not be
    if (vtx.empty() || !vtx[0].IsCoinBase())
        return DoS(100, error("CheckBlock() : first tx is not coinbase"));
    for (unsigned int i = 1; i < vtx.size(); i++)
        if (vtx[i].IsCoinBase())
            return DoS(100, error("CheckBlock() : more than one coinbase"));

    // Check transactions
    BOOST_FOREACH(const CTransaction& tx, vtx)
        if (!tx.CheckTransaction())
            return DoS(tx.nDoS, error("CheckBlock() : CheckTransaction failed"));

    // Check for duplicate txids. This is caught by ConnectInputs(),
    // but catching it earlier avoids a potential DoS attack:
    set<uint256> uniqueTx;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        uniqueTx.insert(tx.GetHash());
    }
    if (uniqueTx.size() != vtx.size())
        return DoS(100, error("CheckBlock() : duplicate transaction"));

    unsigned int nSigOps = 0;
    BOOST_FOREACH(const CTransaction& tx, vtx)
    {
        nSigOps += tx.GetLegacySigOpCount();
    }
    if (nSigOps > MAX_BLOCK_SIGOPS)
        return DoS(100, error("CheckBlock() : out-of-bounds SigOpCount"));

    // Check merkle root
    if (fCheckMerkleRoot && hashMerkleRoot != BuildMerkleTree())
        return DoS(100, error("CheckBlock() : hashMerkleRoot mismatch"));

    return true;
}

bool CBlock::AcceptBlock()
{
    // Check for duplicate
    uint256 hash = GetHash();
    if (mapBlockIndex.count(hash))
        return error("AcceptBlock() : block already in mapBlockIndex");

    // Get prev block index
    map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashPrevBlock);
    if (mi == mapBlockIndex.end())
        return DoS(10, error("AcceptBlock() : prev block not found"));
    CBlockIndex* pindexPrev = (*mi).second;
    int nHeight = pindexPrev->nHeight+1;

    // Check proof of work
    if (nBits != GetNextWorkRequired(pindexPrev, this))
        return DoS(100, error("AcceptBlock() : incorrect proof of work"));

    // Check timestamp against prev
    if (GetBlockTime() <= pindexPrev->GetMedianTimePast())
        return error("AcceptBlock() : block's timestamp is too early");

    // Check that all transactions are finalized
    BOOST_FOREACH(const CTransaction& tx, vtx)
        if (!tx.IsFinal(nHeight, GetBlockTime()))
            return DoS(10, error("AcceptBlock() : contains a non-final transaction"));

    // Check that the block chain matches the known block chain up to a checkpoint
    if (!Checkpoints::CheckBlock(nHeight, hash))
        return DoS(100, error("AcceptBlock() : rejected by checkpoint lock-in at %d", nHeight));

    // Reject block.nVersion=1 blocks when 95% (75% on testnet) of the network has upgraded:
    if (nVersion < 2)
    {
        if ((!fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 950, 1000)) ||
            (fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 75, 100)))
        {
            return error("AcceptBlock() : rejected nVersion=1 block");
        }
    }
    // Enforce block.nVersion=2 rule that the coinbase starts with serialized block height
    if (nVersion >= 2)
    {
        // if 750 of the last 1,000 blocks are version 2 or greater (51/100 if testnet):
        if ((!fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 750, 1000)) ||
            (fTestNet && CBlockIndex::IsSuperMajority(2, pindexPrev, 51, 100)))
        {
            CScript expect = CScript() << nHeight;
            if (!std::equal(expect.begin(), expect.end(), vtx[0].vin[0].scriptSig.begin()))
                return DoS(100, error("AcceptBlock() : block height mismatch in coinbase"));
        }
    }

    // Write block to history file
    if (!CheckDiskSpace(::GetSerializeSize(*this, SER_DISK, CLIENT_VERSION)))
        return error("AcceptBlock() : out of disk space");
    unsigned int nFile = -1;
    unsigned int nBlockPos = 0;
    if (!WriteToDisk(nFile, nBlockPos))
        return error("AcceptBlock() : WriteToDisk failed");
    if (!AddToBlockIndex(nFile, nBlockPos))
        return error("AcceptBlock() : AddToBlockIndex failed");

    // Relay inventory, but don't relay old inventory during initial block download
    int nBlockEstimate = Checkpoints::GetTotalBlocksEstimate();
    if (hashBestChain == hash)
    {
        LOCK(cs_vNodes);
        BOOST_FOREACH(CNode* pnode, vNodes)
            if (nBestHeight > (pnode->nStartingHeight != -1 ? pnode->nStartingHeight - 2000 : nBlockEstimate))
                pnode->PushInventory(CInv(MSG_BLOCK, hash));
    }

    return true;
}

bool CBlockIndex::IsSuperMajority(int minVersion, const CBlockIndex* pstart, unsigned int nRequired, unsigned int nToCheck)
{
    unsigned int nFound = 0;
    for (unsigned int i = 0; i < nToCheck && nFound < nRequired && pstart != NULL; i++)
    {
        if (pstart->nVersion >= minVersion)
            ++nFound;
        pstart = pstart->pprev;
    }
    return (nFound >= nRequired);
}

bool ProcessBlock(CNode* pfrom, CBlock* pblock)
{
    // Check for duplicate
    uint256 hash = pblock->GetHash();
    if (mapBlockIndex.count(hash))
        return error("ProcessBlock() : already have block %d %s", mapBlockIndex[hash]->nHeight, hash.ToString().substr(0,20).c_str());
    if (mapOrphanBlocks.count(hash))
        return error("ProcessBlock() : already have block (orphan) %s", hash.ToString().substr(0,20).c_str());

    // Preliminary checks
    if (!pblock->CheckBlock())
        return error("ProcessBlock() : CheckBlock FAILED");

    CBlockIndex* pcheckpoint = Checkpoints::GetLastCheckpoint(mapBlockIndex);
    if (pcheckpoint && pblock->hashPrevBlock != hashBestChain)
    {
        // Extra checks to prevent "fill up memory by spamming with bogus blocks"
        int64 deltaTime = pblock->GetBlockTime() - pcheckpoint->nTime;
        if (deltaTime < 0)
        {
            if (pfrom)
                pfrom->Misbehaving(100);
            return error("ProcessBlock() : block with timestamp before last checkpoint");
        }
#if 0
        // Now that we are using a FIR filter (see above) this is no longer
        // a straightforward calculation.
        CBigNum bnNewBlock;
        bnNewBlock.SetCompact(pblock->nBits);
        CBigNum bnRequired;
        bnRequired.SetCompact(ComputeMinWork(pcheckpoint->nBits, deltaTime));
        if (bnNewBlock > bnRequired)
        {
            if (pfrom)
                pfrom->Misbehaving(100);
            return error("ProcessBlock() : block with too little proof-of-work");
        }
#endif
    }


    // If we don't already have its previous block, shunt it off to holding area until we get it
    if (!mapBlockIndex.count(pblock->hashPrevBlock))
    {
        printf("ProcessBlock: ORPHAN BLOCK, prev=%s\n", pblock->hashPrevBlock.ToString().substr(0,20).c_str());

        // Accept orphans as long as there is a node to request its parents from
        if (pfrom) {
            CBlock* pblock2 = new CBlock(*pblock);
            mapOrphanBlocks.insert(make_pair(hash, pblock2));
            mapOrphanBlocksByPrev.insert(make_pair(pblock2->hashPrevBlock, pblock2));

            // Ask this guy to fill in what we're missing
            pfrom->PushGetBlocks(pindexBest, GetOrphanRoot(pblock2));
        }
        return true;
    }

    // Store to disk
    if (!pblock->AcceptBlock())
        return error("ProcessBlock() : AcceptBlock FAILED");

    // Recursively process any orphan blocks that depended on this one
    vector<uint256> vWorkQueue;
    vWorkQueue.push_back(hash);
    for (unsigned int i = 0; i < vWorkQueue.size(); i++)
    {
        uint256 hashPrev = vWorkQueue[i];
        for (multimap<uint256, CBlock*>::iterator mi = mapOrphanBlocksByPrev.lower_bound(hashPrev);
             mi != mapOrphanBlocksByPrev.upper_bound(hashPrev);
             ++mi)
        {
            CBlock* pblockOrphan = (*mi).second;
            if (pblockOrphan->AcceptBlock())
                vWorkQueue.push_back(pblockOrphan->GetHash());
            mapOrphanBlocks.erase(pblockOrphan->GetHash());
            delete pblockOrphan;
        }
        mapOrphanBlocksByPrev.erase(hashPrev);
    }

    printf("ProcessBlock: ACCEPTED\n");
    return true;
}








bool CheckDiskSpace(uint64 nAdditionalBytes)
{
    uint64 nFreeBytesAvailable = filesystem::space(GetDataDir()).available;

    // Check for nMinDiskSpace bytes (currently 50MB)
    if (nFreeBytesAvailable < nMinDiskSpace + nAdditionalBytes)
    {
        fShutdown = true;
        string strMessage = _("Warning: Disk space is low!");
        strMiscWarning = strMessage;
        printf("*** %s\n", strMessage.c_str());
        uiInterface.ThreadSafeMessageBox(strMessage, "Xcoin", CClientUIInterface::OK | CClientUIInterface::ICON_EXCLAMATION | CClientUIInterface::MODAL);
        StartShutdown();
        return false;
    }
    return true;
}

static filesystem::path BlockFilePath(unsigned int nFile)
{
    string strBlockFn = strprintf("blk%04u.dat", nFile);
    return GetDataDir() / strBlockFn;
}

FILE* OpenBlockFile(unsigned int nFile, unsigned int nBlockPos, const char* pszMode)
{
    if ((nFile < 1) || (nFile == (unsigned int) -1))
        return NULL;
    FILE* file = fopen(BlockFilePath(nFile).string().c_str(), pszMode);
    if (!file)
        return NULL;
    if (nBlockPos != 0 && !strchr(pszMode, 'a') && !strchr(pszMode, 'w'))
    {
        if (fseek(file, nBlockPos, SEEK_SET) != 0)
        {
            fclose(file);
            return NULL;
        }
    }
    return file;
}

static unsigned int nCurrentBlockFile = 1;

FILE* AppendBlockFile(unsigned int& nFileRet)
{
    nFileRet = 0;
    loop
    {
        FILE* file = OpenBlockFile(nCurrentBlockFile, 0, "ab");
        if (!file)
            return NULL;
        if (fseek(file, 0, SEEK_END) != 0)
            return NULL;
        // FAT32 file size max 4GB, fseek and ftell max 2GB, so we must stay under 2GB
        if (ftell(file) < (long)(0x7F000000 - MAX_SIZE))
        {
            nFileRet = nCurrentBlockFile;
            return file;
        }
        fclose(file);
        nCurrentBlockFile++;
    }
}

bool LoadBlockIndex(bool fAllowNew)
{
    if (fTestNet)
    {
        pchMessageStart[0] = 0x5e;
        pchMessageStart[1] = 0xd6;
        pchMessageStart[2] = 0x7c;
        pchMessageStart[3] = 0xf3;
        hashGenesisBlock = uint256("0x00000000a52504ffe3420a43bd385ef24f81838921a903460b235d95f37cd65e");
    }

    //
    // Load block index
    //
    CTxDB txdb("cr");
    if (!txdb.LoadBlockIndex())
        return false;
    txdb.Close();

    //
    // Init with genesis block
    //
    if (mapBlockIndex.empty())
    {
        if (!fAllowNew)
            return false;

        // Genesis Block:
        // CBlock(hash=000000000019d6, ver=1, hashPrevBlock=00000000000000, hashMerkleRoot=4a5e1e, nTime=1231006505, nBits=1d00ffff, nNonce=2083236893, vtx=1)
        //   CTransaction(hash=4a5e1e, ver=1, vin.size=1, vout.size=1, nLockTime=0)
        //     CTxIn(COutPoint(000000, -1), coinbase 04ffff001d0104455468652054696d65732030332f4a616e2f32303039204368616e63656c6c6f72206f6e206272696e6b206f66207365636f6e64206261696c6f757420666f722062616e6b73)
        //     CTxOut(nValue=50.00000000, scriptPubKey=0x5F1DF16B2B704C8A578D0B)
        //   vMerkleTree: 4a5e1e

        // Genesis block
        const char* pszTimestamp = "Trump is the USA President";
        CTransaction txNew;
        txNew.nVersion = 2;
        txNew.nRefHeight = 0;
        txNew.vin .resize(1);
        txNew.vout.resize(8);
        txNew.vin[0].scriptSig = CScript()
            << 486604799
            << CBigNum(4)
            << vector<unsigned char>(
                   (const unsigned char*)pszTimestamp,
                   (const unsigned char*)pszTimestamp + strlen(pszTimestamp));
        txNew.vout[0].SetInitialValue(25453671561LL);
        txNew.vout[0].scriptPubKey = CScript()
            << ParseHex("04678afdb0fe5548271967f1a67130b7105cd6a828e03909a67962e0ea1f61deb649f6bc3f4cef38c4f35504e51ec112de5c384df7ba0b8d578a4c702b6bf11d5f")
            << OP_CHECKSIG;
        txNew.vout[1].SetInitialValue(1LL);
        txNew.vout[1].scriptPubKey = CScript()
            << uint256("0x000000000000042d1bc432a92c42c186297799da1a7b878d79edc5e080d12950")
            << OP_DROP
            << OP_FALSE;
        const char* pszMessage2 = "\
Metals were an implicitly abusive agreement.\n\
Modern \"paper\" is a flawed tool, its engineering is a nest of leeches.\n\
The old money is obsolete.\n\
Let the individual monetize its credit without cartel intermediaries.\n\
Give us a rent-less cash so we can be free for the first time.\n\
Let this be the awaited dawn.";
        txNew.vout[2].SetInitialValue(1LL);
        txNew.vout[2].scriptPubKey = CScript()
            << ParseHex("202020")
            << OP_DROP
            << vector<unsigned char>(
                   (const unsigned char*)pszMessage2,
                   (const unsigned char*)pszMessage2 + strlen(pszMessage2))
            << OP_DROP
            << OP_DUP
            << OP_HASH160
            << ParseHex("0ef0f9d19a653023554146a866238b8822bc84df")
            << OP_EQUALVERIFY
            << OP_CHECKSIG;
        const char* pszMessage3 = "\
\"Let us calculate, without further ado, in order to see who is right.\" --Gottfried Wilhelm Leibniz\n\
\xce\xbe\xc2\xb4\xef\xbd\xa5\xe2\x88\x80\xef\xbd\xa5`\xef\xbc\x89\xe3\x80\x80\xe3\x80\x80\xe3\x80\x80\xe3\x80\x80  n\n\
\xef\xbf\xa3\xe3\x80\x80\xe3\x80\x80\xe3\x80\x80  \xef\xbc\xbc\xe3\x80\x80\xe3\x80\x80  \xef\xbc\x88 E\xef\xbc\x89 good job, maaku!\n\
\xef\xbe\x8c\xe3\x80\x80\xe3\x80\x80\xe3\x80\x80  /\xe3\x83\xbd \xe3\x83\xbd_\xef\xbc\x8f\xef\xbc\x8f";
        txNew.vout[3].SetInitialValue(1LL);
        txNew.vout[3].scriptPubKey = CScript()
            << ParseHex("2020202020202020")
            << OP_DROP
            << vector<unsigned char>(
                   (const unsigned char*)pszMessage3,
                   (const unsigned char*)pszMessage3 + strlen(pszMessage3))
            << OP_DROP
            << OP_DUP
            << OP_HASH160
            << ParseHex("c26be5ec809aa4bf6b30aa89823cff7cedc3679a")
            << OP_EQUALVERIFY
            << OP_CHECKSIG;
        const char* pszMessage4 = "Ich w\xc3\xbc""nsche Xcoin viel Erfolg zum Nutzen der 99 Prozent!";
        txNew.vout[4].SetInitialValue(1LL);
        txNew.vout[4].scriptPubKey = CScript()
            << ParseHex("202020202020")
            << OP_DROP
            << vector<unsigned char>(
                   (const unsigned char*)pszMessage4,
                   (const unsigned char*)pszMessage4 + strlen(pszMessage4))
            << OP_DROP
            << OP_DUP
            << OP_HASH160
            << ParseHex("2939acd60037281a708eb11e4e9eda452c029eca")
            << OP_EQUALVERIFY
            << OP_CHECKSIG;
        const char* pszMessage5 = "\"The value of a man should be seen in what he gives and not in what he is able to receive.\" --Albert Einstein";
        txNew.vout[5].SetInitialValue(1LL);
        txNew.vout[5].scriptPubKey = CScript()
            << ParseHex("20202020202020202020202020")
            << OP_DROP
            << vector<unsigned char>(
                   (const unsigned char*)pszMessage5,
                   (const unsigned char*)pszMessage5 + strlen(pszMessage5))
            << OP_DROP
            << OP_DUP
            << OP_HASH160
            << ParseHex("f9ca5caab4bda4dc28b5556aa79a2eec0447f0bf")
            << OP_EQUALVERIFY
            << OP_CHECKSIG;
        const char* pszMessage6 = "\"An army of principles can penetrate where an army of soldiers cannot.\" --Thomas Paine";
        txNew.vout[6].SetInitialValue(1LL);
        txNew.vout[6].scriptPubKey = CScript()
            << ParseHex("202020202020202020202020")
            << OP_DROP
            << vector<unsigned char>(
                   (const unsigned char*)pszMessage6,
                   (const unsigned char*)pszMessage6 + strlen(pszMessage6))
            << OP_DROP
            << OP_DUP
            << OP_HASH160
            << ParseHex("08f320cbb41a1ae25b794f6175f96080681989f3")
            << OP_EQUALVERIFY
            << OP_CHECKSIG;
        txNew.vout[7].SetInitialValue(49603174604LL);
        txNew.vout[7].scriptPubKey = CScript()
            << OP_DUP
            << OP_HASH160
            << ParseHex("85e54144c4020a65fa0a8fdbac8bba75dbc2fd00")
            << OP_EQUALVERIFY
            << OP_CHECKSIG;
        CBlock block;
        block.vtx.push_back(txNew);
        block.hashPrevBlock  = 0;
        block.hashMerkleRoot = block.BuildMerkleTree();
        block.nVersion = 1;
        block.nTime    = 1498852900;
        block.nBits    = 0x1d00ffff;
        block.nNonce   =  278229610;

        if (fTestNet)
        {
            block.nTime    = 1498852900;
            block.nNonce   = 3098244593;
        }

        //// debug print
        printf("%s\n", block.GetHash().ToString().c_str());
        printf("%s\n", hashGenesisBlock.ToString().c_str());
        printf("%s\n", block.hashMerkleRoot.ToString().c_str());
        assert(block.hashMerkleRoot == uint256("0xf53b1baa971ea40be88cf51288aabd700dfec96c486bf7155a53a4919af4c8bd"));
        block.print();
        assert(block.GetHash() == hashGenesisBlock);

        // Start new block file
        unsigned int nFile;
        unsigned int nBlockPos;
        if (!block.WriteToDisk(nFile, nBlockPos))
            return error("LoadBlockIndex() : writing genesis block to disk failed");
        if (!block.AddToBlockIndex(nFile, nBlockPos))
            return error("LoadBlockIndex() : genesis block not accepted");
    }

    return true;
}



void PrintBlockTree()
{
    // pre-compute tree structure
    map<CBlockIndex*, vector<CBlockIndex*> > mapNext;
    for (map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.begin(); mi != mapBlockIndex.end(); ++mi)
    {
        CBlockIndex* pindex = (*mi).second;
        mapNext[pindex->pprev].push_back(pindex);
        // test
        //while (rand() % 3 == 0)
        //    mapNext[pindex->pprev].push_back(pindex);
    }

    vector<pair<int, CBlockIndex*> > vStack;
    vStack.push_back(make_pair(0, pindexGenesisBlock));

    int nPrevCol = 0;
    while (!vStack.empty())
    {
        int nCol = vStack.back().first;
        CBlockIndex* pindex = vStack.back().second;
        vStack.pop_back();

        // print split or gap
        if (nCol > nPrevCol)
        {
            for (int i = 0; i < nCol-1; i++)
                printf("| ");
            printf("|\\\n");
        }
        else if (nCol < nPrevCol)
        {
            for (int i = 0; i < nCol; i++)
                printf("| ");
            printf("|\n");
       }
        nPrevCol = nCol;

        // print columns
        for (int i = 0; i < nCol; i++)
            printf("| ");

        // print item
        CBlock block;
        block.ReadFromDisk(pindex);
        printf("%d (%u,%u) %s  %s  tx %"PRIszu"",
            pindex->nHeight,
            pindex->nFile,
            pindex->nBlockPos,
            block.GetHash().ToString().substr(0,20).c_str(),
            DateTimeStrFormat("%x %H:%M:%S", block.GetBlockTime()).c_str(),
            block.vtx.size());

        PrintWallets(block);

        // put the main time-chain first
        vector<CBlockIndex*>& vNext = mapNext[pindex];
        for (unsigned int i = 0; i < vNext.size(); i++)
        {
            if (vNext[i]->pnext)
            {
                swap(vNext[0], vNext[i]);
                break;
            }
        }

        // iterate children
        for (unsigned int i = 0; i < vNext.size(); i++)
            vStack.push_back(make_pair(nCol+i, vNext[i]));
    }
}

bool LoadExternalBlockFile(FILE* fileIn)
{
    int64 nStart = GetTimeMillis();

    int nLoaded = 0;
    {
        LOCK(cs_main);
        try {
            CAutoFile blkdat(fileIn, SER_DISK, CLIENT_VERSION);
            unsigned int nPos = 0;
            while (nPos != (unsigned int)-1 && blkdat.good() && !fRequestShutdown)
            {
                unsigned char pchData[65536];
                do {
                    fseek(blkdat, nPos, SEEK_SET);
                    int nRead = fread(pchData, 1, sizeof(pchData), blkdat);
                    if (nRead <= 8)
                    {
                        nPos = (unsigned int)-1;
                        break;
                    }
                    void* nFind = memchr(pchData, pchMessageStart[0], nRead+1-sizeof(pchMessageStart));
                    if (nFind)
                    {
                        if (memcmp(nFind, pchMessageStart, sizeof(pchMessageStart))==0)
                        {
                            nPos += ((unsigned char*)nFind - pchData) + sizeof(pchMessageStart);
                            break;
                        }
                        nPos += ((unsigned char*)nFind - pchData) + 1;
                    }
                    else
                        nPos += sizeof(pchData) - sizeof(pchMessageStart) + 1;
                } while(!fRequestShutdown);
                if (nPos == (unsigned int)-1)
                    break;
                fseek(blkdat, nPos, SEEK_SET);
                unsigned int nSize;
                blkdat >> nSize;
                if (nSize > 0 && nSize <= MAX_BLOCK_SIZE)
                {
                    CBlock block;
                    blkdat >> block;
                    if (ProcessBlock(NULL,&block))
                    {
                        nLoaded++;
                        nPos += 4 + nSize;
                    }
                }
            }
        }
        catch (std::exception &e) {
            printf("%s() : Deserialize or I/O error caught during load\n",
                   __PRETTY_FUNCTION__);
        }
    }
    printf("Loaded %i blocks from external file in %"PRI64d"ms\n", nLoaded, GetTimeMillis() - nStart);
    return nLoaded > 0;
}









//////////////////////////////////////////////////////////////////////////////
//
// CAlert
//

extern map<uint256, CAlert> mapAlerts;
extern CCriticalSection cs_mapAlerts;

string GetWarnings(string strFor)
{
    int nPriority = 0;
    string strStatusBar;
    string strRPC;
    if (GetBoolArg("-testsafemode"))
        strRPC = "test";

    // Misc warnings like out of disk space and clock is wrong
    if (strMiscWarning != "")
    {
        nPriority = 1000;
        strStatusBar = strMiscWarning;
    }

    // Longer invalid proof-of-work chain
    if (pindexBest && bnBestInvalidWork > bnBestChainWork + pindexBest->GetBlockWork() * 6)
    {
        nPriority = 2000;
        strStatusBar = strRPC = _("Warning: Displayed transactions may not be correct! You may need to upgrade, or other nodes may need to upgrade.");
    }

    // Alerts
    {
        LOCK(cs_mapAlerts);
        BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
        {
            const CAlert& alert = item.second;
            if (alert.AppliesToMe() && alert.nPriority > nPriority)
            {
                nPriority = alert.nPriority;
                strStatusBar = alert.strStatusBar;
            }
        }
    }

    if (strFor == "statusbar")
        return strStatusBar;
    else if (strFor == "rpc")
        return strRPC;
    assert(!"GetWarnings() : invalid parameter");
    return "error";
}








//////////////////////////////////////////////////////////////////////////////
//
// Messages
//


bool static AlreadyHave(CTxDB& txdb, const CInv& inv)
{
    switch (inv.type)
    {
    case MSG_TX:
        {
        bool txInMap = false;
            {
            LOCK(mempool.cs);
            txInMap = (mempool.exists(inv.hash));
            }
        return txInMap ||
               mapOrphanTransactions.count(inv.hash) ||
               txdb.ContainsTx(inv.hash);
        }

    case MSG_BLOCK:
        return mapBlockIndex.count(inv.hash) ||
               mapOrphanBlocks.count(inv.hash);
    }
    // Don't know what it is, just say we already got one
    return true;
}




// The message start string is designed to be unlikely to occur in normal data.
// The characters are rarely used upper ASCII, not valid as UTF-8, and produce
// a large 4-byte int at any alignment.
unsigned char pchMessageStart[4] = { 0x2c, 0xfe, 0x7e, 0x6d };


bool static ProcessMessage(CNode* pfrom, string strCommand, CDataStream& vRecv)
{
    static map<CService, CPubKey> mapReuseKey;
    RandAddSeedPerfmon();
    if (fDebug)
        printf("received: %s (%"PRIszu" bytes)\n", strCommand.c_str(), vRecv.size());
    if (mapArgs.count("-dropmessagestest") && GetRand(atoi(mapArgs["-dropmessagestest"])) == 0)
    {
        printf("dropmessagestest DROPPING RECV MESSAGE\n");
        return true;
    }





    if (strCommand == "version")
    {
        // Each connection can only send one version message
        if (pfrom->nVersion != 0)
        {
            pfrom->Misbehaving(1);
            return false;
        }

        int64 nTime;
        CAddress addrMe;
        CAddress addrFrom;
        uint64 nNonce = 1;
        vRecv >> pfrom->nVersion >> pfrom->nServices >> nTime >> addrMe;
        if (pfrom->nVersion < MIN_PROTO_VERSION)
        {
            // Since February 20, 2012, the protocol is initiated at version 209,
            // and earlier versions are no longer supported
            printf("partner %s using obsolete version %i; disconnecting\n", pfrom->addr.ToString().c_str(), pfrom->nVersion);
            pfrom->fDisconnect = true;
            return false;
        }

        if (pfrom->nVersion == 10300)
            pfrom->nVersion = 300;
        if (!vRecv.empty())
            vRecv >> addrFrom >> nNonce;
        if (!vRecv.empty())
            vRecv >> pfrom->strSubVer;
        if (!vRecv.empty())
            vRecv >> pfrom->nStartingHeight;

        if (pfrom->fInbound && addrMe.IsRoutable())
        {
            pfrom->addrLocal = addrMe;
            SeenLocal(addrMe);
        }

        // Disconnect if we connected to ourself
        if (nNonce == nLocalHostNonce && nNonce > 1)
        {
            printf("connected to self at %s, disconnecting\n", pfrom->addr.ToString().c_str());
            pfrom->fDisconnect = true;
            return true;
        }

        // Be shy and don't send version until we hear
        if (pfrom->fInbound)
            pfrom->PushVersion();

        pfrom->fClient = !(pfrom->nServices & NODE_NETWORK);

        AddTimeData(pfrom->addr, nTime);

        // Change version
        pfrom->PushMessage("verack");
        pfrom->vSend.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));

        if (!pfrom->fInbound)
        {
            // Advertise our address
            if (!fNoListen && !IsInitialBlockDownload())
            {
                CAddress addr = GetLocalAddress(&pfrom->addr);
                if (addr.IsRoutable())
                    pfrom->PushAddress(addr);
            }

            // Get recent addresses
            if (pfrom->fOneShot || pfrom->nVersion >= CADDR_TIME_VERSION || addrman.size() < 1000)
            {
                pfrom->PushMessage("getaddr");
                pfrom->fGetAddr = true;
            }
            addrman.Good(pfrom->addr);
        } else {
            if (((CNetAddr)pfrom->addr) == (CNetAddr)addrFrom)
            {
                addrman.Add(addrFrom, addrFrom);
                addrman.Good(addrFrom);
            }
        }

        // Ask the first connected node for block updates
        static int nAskedForBlocks = 0;
        if (!pfrom->fClient && !pfrom->fOneShot &&
            (pfrom->nStartingHeight > (nBestHeight - 144)) &&
            (pfrom->nVersion < NOBLKS_VERSION_START ||
             pfrom->nVersion >= NOBLKS_VERSION_END) &&
             (nAskedForBlocks < 1 || vNodes.size() <= 1))
        {
            nAskedForBlocks++;
            pfrom->PushGetBlocks(pindexBest, uint256(0));
        }

        // Relay alerts
        {
            LOCK(cs_mapAlerts);
            BOOST_FOREACH(PAIRTYPE(const uint256, CAlert)& item, mapAlerts)
                item.second.RelayTo(pfrom);
        }

        pfrom->fSuccessfullyConnected = true;

        printf("receive version message: version %d, blocks=%d, us=%s, them=%s, peer=%s\n", pfrom->nVersion, pfrom->nStartingHeight, addrMe.ToString().c_str(), addrFrom.ToString().c_str(), pfrom->addr.ToString().c_str());

        cPeerBlockCounts.input(pfrom->nStartingHeight);
    }


    else if (pfrom->nVersion == 0)
    {
        // Must have a version message before anything else
        pfrom->Misbehaving(1);
        return false;
    }


    else if (strCommand == "verack")
    {
        pfrom->vRecv.SetVersion(min(pfrom->nVersion, PROTOCOL_VERSION));
    }


    else if (strCommand == "addr")
    {
        vector<CAddress> vAddr;
        vRecv >> vAddr;

        // Don't want addr from older versions unless seeding
        if (pfrom->nVersion < CADDR_TIME_VERSION && addrman.size() > 1000)
            return true;
        if (vAddr.size() > 1000)
        {
            pfrom->Misbehaving(20);
            return error("message addr size() = %"PRIszu"", vAddr.size());
        }

        // Store the new addresses
        vector<CAddress> vAddrOk;
        int64 nNow = GetAdjustedTime();
        int64 nSince = nNow - 10 * 60;
        BOOST_FOREACH(CAddress& addr, vAddr)
        {
            if (fShutdown)
                return true;
            if (addr.nTime <= 100000000 || addr.nTime > nNow + 10 * 60)
                addr.nTime = nNow - 5 * 24 * 60 * 60;
            pfrom->AddAddressKnown(addr);
            bool fReachable = IsReachable(addr);
            if (addr.nTime > nSince && !pfrom->fGetAddr && vAddr.size() <= 10 && addr.IsRoutable())
            {
                // Relay to a limited number of other nodes
                {
                    LOCK(cs_vNodes);
                    // Use deterministic randomness to send to the same nodes for 24 hours
                    // at a time so the setAddrKnowns of the chosen nodes prevent repeats
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint64 hashAddr = addr.GetHash();
                    uint256 hashRand = hashSalt ^ (hashAddr<<32) ^ ((GetTime()+hashAddr)/(24*60*60));
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    multimap<uint256, CNode*> mapMix;
                    BOOST_FOREACH(CNode* pnode, vNodes)
                    {
                        if (pnode->nVersion < CADDR_TIME_VERSION)
                            continue;
                        unsigned int nPointer;
                        memcpy(&nPointer, &pnode, sizeof(nPointer));
                        uint256 hashKey = hashRand ^ nPointer;
                        hashKey = Hash(BEGIN(hashKey), END(hashKey));
                        mapMix.insert(make_pair(hashKey, pnode));
                    }
                    int nRelayNodes = fReachable ? 2 : 1; // limited relaying of addresses outside our network(s)
                    for (multimap<uint256, CNode*>::iterator mi = mapMix.begin(); mi != mapMix.end() && nRelayNodes-- > 0; ++mi)
                        ((*mi).second)->PushAddress(addr);
                }
            }
            // Do not store addresses outside our network
            if (fReachable)
                vAddrOk.push_back(addr);
        }
        addrman.Add(vAddrOk, pfrom->addr, 2 * 60 * 60);
        if (vAddr.size() < 1000)
            pfrom->fGetAddr = false;
        if (pfrom->fOneShot)
            pfrom->fDisconnect = true;
    }


    else if (strCommand == "inv")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            pfrom->Misbehaving(20);
            return error("message inv size() = %"PRIszu"", vInv.size());
        }

        // find last block in inv vector
        unsigned int nLastBlock = (unsigned int)(-1);
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++) {
            if (vInv[vInv.size() - 1 - nInv].type == MSG_BLOCK) {
                nLastBlock = vInv.size() - 1 - nInv;
                break;
            }
        }
        CTxDB txdb("r");
        for (unsigned int nInv = 0; nInv < vInv.size(); nInv++)
        {
            const CInv &inv = vInv[nInv];

            if (fShutdown)
                return true;
            pfrom->AddInventoryKnown(inv);

            bool fAlreadyHave = AlreadyHave(txdb, inv);
            if (fDebug)
                printf("  got inventory: %s  %s\n", inv.ToString().c_str(), fAlreadyHave ? "have" : "new");

            if (!fAlreadyHave)
                pfrom->AskFor(inv);
            else if (inv.type == MSG_BLOCK && mapOrphanBlocks.count(inv.hash)) {
                pfrom->PushGetBlocks(pindexBest, GetOrphanRoot(mapOrphanBlocks[inv.hash]));
            } else if (nInv == nLastBlock) {
                // In case we are on a very long side-chain, it is possible that we already have
                // the last block in an inv bundle sent in response to getblocks. Try to detect
                // this situation and push another getblocks to continue.
                pfrom->PushGetBlocks(mapBlockIndex[inv.hash], uint256(0));
                if (fDebug)
                    printf("force request: %s\n", inv.ToString().c_str());
            }

            // Track requests for our stuff
            Inventory(inv.hash);
        }
    }


    else if (strCommand == "getdata")
    {
        vector<CInv> vInv;
        vRecv >> vInv;
        if (vInv.size() > MAX_INV_SZ)
        {
            pfrom->Misbehaving(20);
            return error("message getdata size() = %"PRIszu"", vInv.size());
        }

        if (fDebugNet || (vInv.size() != 1))
            printf("received getdata (%"PRIszu" invsz)\n", vInv.size());

        BOOST_FOREACH(const CInv& inv, vInv)
        {
            if (fShutdown)
                return true;
            if (fDebugNet || (vInv.size() == 1))
                printf("received getdata for: %s\n", inv.ToString().c_str());

            if (inv.type == MSG_BLOCK)
            {
                // Send block from disk
                map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(inv.hash);
                if (mi != mapBlockIndex.end())
                {
                    CBlock block;
                    block.ReadFromDisk((*mi).second);
                    pfrom->PushMessage("block", block);

                    // Trigger them to send a getblocks request for the next batch of inventory
                    if (inv.hash == pfrom->hashContinue)
                    {
                        // Bypass PushInventory, this must send even if redundant,
                        // and we want it right after the last block so they don't
                        // wait for other stuff first.
                        vector<CInv> vInv;
                        vInv.push_back(CInv(MSG_BLOCK, hashBestChain));
                        pfrom->PushMessage("inv", vInv);
                        pfrom->hashContinue = 0;
                    }
                }
            }
            else if (inv.IsKnownType())
            {
                // Send stream from relay memory
                bool pushed = false;
                {
                    LOCK(cs_mapRelay);
                    map<CInv, CDataStream>::iterator mi = mapRelay.find(inv);
                    if (mi != mapRelay.end()) {
                        pfrom->PushMessage(inv.GetCommand(), (*mi).second);
                        pushed = true;
                    }
                }
                if (!pushed && inv.type == MSG_TX) {
                    LOCK(mempool.cs);
                    if (mempool.exists(inv.hash)) {
                        CTransaction tx = mempool.lookup(inv.hash);
                        CDataStream ss(SER_NETWORK, PROTOCOL_VERSION);
                        ss.reserve(1000);
                        ss << tx;
                        pfrom->PushMessage("tx", ss);
                    }
                }
            }

            // Track requests for our stuff
            Inventory(inv.hash);
        }
    }


    else if (strCommand == "getblocks")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        // Find the last block the caller has in the main chain
        CBlockIndex* pindex = locator.GetBlockIndex();

        // Send the rest of the chain
        if (pindex)
            pindex = pindex->pnext;
        int nLimit = 500;
        printf("getblocks %d to %s limit %d\n", (pindex ? pindex->nHeight : -1), hashStop.ToString().substr(0,20).c_str(), nLimit);
        for (; pindex; pindex = pindex->pnext)
        {
            if (pindex->GetBlockHash() == hashStop)
            {
                printf("  getblocks stopping at %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString().substr(0,20).c_str());
                break;
            }
            pfrom->PushInventory(CInv(MSG_BLOCK, pindex->GetBlockHash()));
            if (--nLimit <= 0)
            {
                // When this block is requested, we'll send an inv that'll make them
                // getblocks the next batch of inventory.
                printf("  getblocks stopping at limit %d %s\n", pindex->nHeight, pindex->GetBlockHash().ToString().substr(0,20).c_str());
                pfrom->hashContinue = pindex->GetBlockHash();
                break;
            }
        }
    }


    else if (strCommand == "getheaders")
    {
        CBlockLocator locator;
        uint256 hashStop;
        vRecv >> locator >> hashStop;

        CBlockIndex* pindex = NULL;
        if (locator.IsNull())
        {
            // If locator is null, return the hashStop block
            map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashStop);
            if (mi == mapBlockIndex.end())
                return true;
            pindex = (*mi).second;
        }
        else
        {
            // Find the last block the caller has in the main chain
            pindex = locator.GetBlockIndex();
            if (pindex)
                pindex = pindex->pnext;
        }

        vector<CBlock> vHeaders;
        int nLimit = 2000;
        printf("getheaders %d to %s\n", (pindex ? pindex->nHeight : -1), hashStop.ToString().substr(0,20).c_str());
        for (; pindex; pindex = pindex->pnext)
        {
            vHeaders.push_back(pindex->GetBlockHeader());
            if (--nLimit <= 0 || pindex->GetBlockHash() == hashStop)
                break;
        }
        pfrom->PushMessage("headers", vHeaders);
    }


    else if (strCommand == "tx")
    {
        vector<uint256> vWorkQueue;
        vector<uint256> vEraseQueue;
        CDataStream vMsg(vRecv);
        CTxDB txdb("r");
        CTransaction tx;
        vRecv >> tx;

        CInv inv(MSG_TX, tx.GetHash());
        pfrom->AddInventoryKnown(inv);

        bool fMissingInputs = false;
        if (tx.AcceptToMemoryPool(txdb, true, &fMissingInputs))
        {
            SyncWithWallets(tx, NULL, true);
            RelayMessage(inv, vMsg);
            mapAlreadyAskedFor.erase(inv);
            vWorkQueue.push_back(inv.hash);
            vEraseQueue.push_back(inv.hash);

            // Recursively process any orphan transactions that depended on this one
            for (unsigned int i = 0; i < vWorkQueue.size(); i++)
            {
                uint256 hashPrev = vWorkQueue[i];
                for (map<uint256, CDataStream*>::iterator mi = mapOrphanTransactionsByPrev[hashPrev].begin();
                     mi != mapOrphanTransactionsByPrev[hashPrev].end();
                     ++mi)
                {
                    const CDataStream& vMsg = *((*mi).second);
                    CTransaction tx;
                    CDataStream(vMsg) >> tx;
                    CInv inv(MSG_TX, tx.GetHash());
                    bool fMissingInputs2 = false;

                    if (tx.AcceptToMemoryPool(txdb, true, &fMissingInputs2))
                    {
                        printf("   accepted orphan tx %s\n", inv.hash.ToString().substr(0,10).c_str());
                        SyncWithWallets(tx, NULL, true);
                        RelayMessage(inv, vMsg);
                        mapAlreadyAskedFor.erase(inv);
                        vWorkQueue.push_back(inv.hash);
                        vEraseQueue.push_back(inv.hash);
                    }
                    else if (!fMissingInputs2)
                    {
                        // invalid orphan
                        vEraseQueue.push_back(inv.hash);
                        printf("   removed invalid orphan tx %s\n", inv.hash.ToString().substr(0,10).c_str());
                    }
                }
            }

            BOOST_FOREACH(uint256 hash, vEraseQueue)
                EraseOrphanTx(hash);
        }
        else if (fMissingInputs)
        {
            AddOrphanTx(vMsg);

            // DoS prevention: do not allow mapOrphanTransactions to grow unbounded
            unsigned int nEvicted = LimitOrphanTxSize(MAX_ORPHAN_TRANSACTIONS);
            if (nEvicted > 0)
                printf("mapOrphan overflow, removed %u tx\n", nEvicted);
        }
        if (tx.nDoS) pfrom->Misbehaving(tx.nDoS);
    }


    else if (strCommand == "block")
    {
        CBlock block;
        vRecv >> block;

        printf("received block %s\n", block.GetHash().ToString().substr(0,20).c_str());
        // block.print();

        CInv inv(MSG_BLOCK, block.GetHash());
        pfrom->AddInventoryKnown(inv);

        if (ProcessBlock(pfrom, &block))
            mapAlreadyAskedFor.erase(inv);
        if (block.nDoS) pfrom->Misbehaving(block.nDoS);
    }


    else if (strCommand == "getaddr")
    {
        pfrom->vAddrToSend.clear();
        vector<CAddress> vAddr = addrman.GetAddr();
        BOOST_FOREACH(const CAddress &addr, vAddr)
            pfrom->PushAddress(addr);
    }


    else if (strCommand == "mempool")
    {
        std::vector<uint256> vtxid;
        mempool.queryHashes(vtxid);
        vector<CInv> vInv;
        for (unsigned int i = 0; i < vtxid.size(); i++) {
            CInv inv(MSG_TX, vtxid[i]);
            vInv.push_back(inv);
            if (i == (MAX_INV_SZ - 1))
                    break;
        }
        if (vInv.size() > 0)
            pfrom->PushMessage("inv", vInv);
    }


    else if (strCommand == "checkorder")
    {
        uint256 hashReply;
        vRecv >> hashReply;

        if (!GetBoolArg("-allowreceivebyip"))
        {
            pfrom->PushMessage("reply", hashReply, (int)2, string(""));
            return true;
        }

        CWalletTx order;
        vRecv >> order;

        /// we have a chance to check the order here

        // Keep giving the same key to the same ip until they use it
        if (!mapReuseKey.count(pfrom->addr))
            pwalletMain->GetKeyFromPool(mapReuseKey[pfrom->addr], true);

        // Send back approval of order and pubkey to use
        CScript scriptPubKey;
        scriptPubKey << mapReuseKey[pfrom->addr] << OP_CHECKSIG;
        pfrom->PushMessage("reply", hashReply, (int)0, scriptPubKey);
    }


    else if (strCommand == "reply")
    {
        uint256 hashReply;
        vRecv >> hashReply;

        CRequestTracker tracker;
        {
            LOCK(pfrom->cs_mapRequests);
            map<uint256, CRequestTracker>::iterator mi = pfrom->mapRequests.find(hashReply);
            if (mi != pfrom->mapRequests.end())
            {
                tracker = (*mi).second;
                pfrom->mapRequests.erase(mi);
            }
        }
        if (!tracker.IsNull())
            tracker.fn(tracker.param1, vRecv);
    }


    else if (strCommand == "ping")
    {
        if (pfrom->nVersion > BIP0031_VERSION)
        {
            uint64 nonce = 0;
            vRecv >> nonce;
            // Echo the message back with the nonce. This allows for two useful features:
            //
            // 1) A remote node can quickly check if the connection is operational
            // 2) Remote nodes can measure the latency of the network thread. If this node
            //    is overloaded it won't respond to pings quickly and the remote node can
            //    avoid sending us more work, like chain download requests.
            //
            // The nonce stops the remote getting confused between different pings: without
            // it, if the remote node sends a ping once per second and this node takes 5
            // seconds to respond to each, the 5th ping the remote sends would appear to
            // return very quickly.
            pfrom->PushMessage("pong", nonce);
        }
    }


    else if (strCommand == "alert")
    {
        CAlert alert;
        vRecv >> alert;

        uint256 alertHash = alert.GetHash();
        if (pfrom->setKnown.count(alertHash) == 0)
        {
            if (alert.ProcessAlert())
            {
                // Relay
                pfrom->setKnown.insert(alertHash);
                {
                    LOCK(cs_vNodes);
                    BOOST_FOREACH(CNode* pnode, vNodes)
                        alert.RelayTo(pnode);
                }
            }
            else {
                // Small DoS penalty so peers that send us lots of
                // duplicate/expired/invalid-signature/whatever alerts
                // eventually get banned.
                // This isn't a Misbehaving(100) (immediate ban) because the
                // peer might be an older or different implementation with
                // a different signature key, etc.
                pfrom->Misbehaving(10);
            }
        }
    }


    else
    {
        // Ignore unknown commands for extensibility
    }


    // Update the last seen time for this node's address
    if (pfrom->fNetworkNode)
        if (strCommand == "version" || strCommand == "addr" || strCommand == "inv" || strCommand == "getdata" || strCommand == "ping")
            AddressCurrentlyConnected(pfrom->addr);


    return true;
}

bool ProcessMessages(CNode* pfrom)
{
    CDataStream& vRecv = pfrom->vRecv;
    if (vRecv.empty())
        return true;
    //if (fDebug)
    //    printf("ProcessMessages(%u bytes)\n", vRecv.size());

    //
    // Message format
    //  (4) message start
    //  (12) command
    //  (4) size
    //  (4) checksum
    //  (x) data
    //

    loop
    {
        // Don't bother if send buffer is too full to respond anyway
        if (pfrom->vSend.size() >= SendBufferSize())
            break;

        // Scan for message start
        CDataStream::iterator pstart = search(vRecv.begin(), vRecv.end(), BEGIN(pchMessageStart), END(pchMessageStart));
        int nHeaderSize = vRecv.GetSerializeSize(CMessageHeader());
        if (vRecv.end() - pstart < nHeaderSize)
        {
            if ((int)vRecv.size() > nHeaderSize)
            {
                printf("\n\nPROCESSMESSAGE MESSAGESTART NOT FOUND\n\n");
                vRecv.erase(vRecv.begin(), vRecv.end() - nHeaderSize);
            }
            break;
        }
        if (pstart - vRecv.begin() > 0)
            printf("\n\nPROCESSMESSAGE SKIPPED %"PRIpdd" BYTES\n\n", pstart - vRecv.begin());
        vRecv.erase(vRecv.begin(), pstart);

        // Read header
        vector<char> vHeaderSave(vRecv.begin(), vRecv.begin() + nHeaderSize);
        CMessageHeader hdr;
        vRecv >> hdr;
        if (!hdr.IsValid())
        {
            printf("\n\nPROCESSMESSAGE: ERRORS IN HEADER %s\n\n\n", hdr.GetCommand().c_str());
            continue;
        }
        string strCommand = hdr.GetCommand();

        // Message size
        unsigned int nMessageSize = hdr.nMessageSize;
        if (nMessageSize > MAX_SIZE)
        {
            printf("ProcessMessages(%s, %u bytes) : nMessageSize > MAX_SIZE\n", strCommand.c_str(), nMessageSize);
            continue;
        }
        if (nMessageSize > vRecv.size())
        {
            // Rewind and wait for rest of message
            vRecv.insert(vRecv.begin(), vHeaderSave.begin(), vHeaderSave.end());
            break;
        }

        // Checksum
        uint256 hash = Hash(vRecv.begin(), vRecv.begin() + nMessageSize);
        unsigned int nChecksum = 0;
        memcpy(&nChecksum, &hash, sizeof(nChecksum));
        if (nChecksum != hdr.nChecksum)
        {
            printf("ProcessMessages(%s, %u bytes) : CHECKSUM ERROR nChecksum=%08x hdr.nChecksum=%08x\n",
               strCommand.c_str(), nMessageSize, nChecksum, hdr.nChecksum);
            continue;
        }

        // Copy message to its own buffer
        CDataStream vMsg(vRecv.begin(), vRecv.begin() + nMessageSize, vRecv.nType, vRecv.nVersion);
        vRecv.ignore(nMessageSize);

        // Process message
        bool fRet = false;
        try
        {
            {
                LOCK(cs_main);
                fRet = ProcessMessage(pfrom, strCommand, vMsg);
            }
            if (fShutdown)
                return true;
        }
        catch (std::ios_base::failure& e)
        {
            if (strstr(e.what(), "end of data"))
            {
                // Allow exceptions from under-length message on vRecv
                printf("ProcessMessages(%s, %u bytes) : Exception '%s' caught, normally caused by a message being shorter than its stated length\n", strCommand.c_str(), nMessageSize, e.what());
            }
            else if (strstr(e.what(), "size too large"))
            {
                // Allow exceptions from over-long size
                printf("ProcessMessages(%s, %u bytes) : Exception '%s' caught\n", strCommand.c_str(), nMessageSize, e.what());
            }
            else
            {
                PrintExceptionContinue(&e, "ProcessMessages()");
            }
        }
        catch (std::exception& e) {
            PrintExceptionContinue(&e, "ProcessMessages()");
        } catch (...) {
            PrintExceptionContinue(NULL, "ProcessMessages()");
        }

        if (!fRet)
            printf("ProcessMessage(%s, %u bytes) FAILED\n", strCommand.c_str(), nMessageSize);
    }

    vRecv.Compact();
    return true;
}


bool SendMessages(CNode* pto, bool fSendTrickle)
{
    TRY_LOCK(cs_main, lockMain);
    if (lockMain) {
        // Don't send anything until we get their version message
        if (pto->nVersion == 0)
            return true;

        // Keep-alive ping. We send a nonce of zero because we don't use it anywhere
        // right now.
        if (pto->nLastSend && GetTime() - pto->nLastSend > 30 * 60 && pto->vSend.empty()) {
            uint64 nonce = 0;
            if (pto->nVersion > BIP0031_VERSION)
                pto->PushMessage("ping", nonce);
            else
                pto->PushMessage("ping");
        }

        // Resend wallet transactions that haven't gotten in a block yet
        ResendWalletTransactions();

        // Address refresh broadcast
        static int64 nLastRebroadcast;
        if (!IsInitialBlockDownload() && (GetTime() - nLastRebroadcast > 24 * 60 * 60))
        {
            {
                LOCK(cs_vNodes);
                BOOST_FOREACH(CNode* pnode, vNodes)
                {
                    // Periodically clear setAddrKnown to allow refresh broadcasts
                    if (nLastRebroadcast)
                        pnode->setAddrKnown.clear();

                    // Rebroadcast our address
                    if (!fNoListen)
                    {
                        CAddress addr = GetLocalAddress(&pnode->addr);
                        if (addr.IsRoutable())
                            pnode->PushAddress(addr);
                    }
                }
            }
            nLastRebroadcast = GetTime();
        }

        //
        // Message: addr
        //
        if (fSendTrickle)
        {
            vector<CAddress> vAddr;
            vAddr.reserve(pto->vAddrToSend.size());
            BOOST_FOREACH(const CAddress& addr, pto->vAddrToSend)
            {
                // returns true if wasn't already contained in the set
                if (pto->setAddrKnown.insert(addr).second)
                {
                    vAddr.push_back(addr);
                    // receiver rejects addr messages larger than 1000
                    if (vAddr.size() >= 1000)
                    {
                        pto->PushMessage("addr", vAddr);
                        vAddr.clear();
                    }
                }
            }
            pto->vAddrToSend.clear();
            if (!vAddr.empty())
                pto->PushMessage("addr", vAddr);
        }


        //
        // Message: inventory
        //
        vector<CInv> vInv;
        vector<CInv> vInvWait;
        {
            LOCK(pto->cs_inventory);
            vInv.reserve(pto->vInventoryToSend.size());
            vInvWait.reserve(pto->vInventoryToSend.size());
            BOOST_FOREACH(const CInv& inv, pto->vInventoryToSend)
            {
                if (pto->setInventoryKnown.count(inv))
                    continue;

                // trickle out tx inv to protect privacy
                if (inv.type == MSG_TX && !fSendTrickle)
                {
                    // 1/4 of tx invs blast to all immediately
                    static uint256 hashSalt;
                    if (hashSalt == 0)
                        hashSalt = GetRandHash();
                    uint256 hashRand = inv.hash ^ hashSalt;
                    hashRand = Hash(BEGIN(hashRand), END(hashRand));
                    bool fTrickleWait = ((hashRand & 3) != 0);

                    // always trickle our own transactions
                    if (!fTrickleWait)
                    {
                        CWalletTx wtx;
                        if (GetTransaction(inv.hash, wtx))
                            if (wtx.fFromMe)
                                fTrickleWait = true;
                    }

                    if (fTrickleWait)
                    {
                        vInvWait.push_back(inv);
                        continue;
                    }
                }

                // returns true if wasn't already contained in the set
                if (pto->setInventoryKnown.insert(inv).second)
                {
                    vInv.push_back(inv);
                    if (vInv.size() >= 1000)
                    {
                        pto->PushMessage("inv", vInv);
                        vInv.clear();
                    }
                }
            }
            pto->vInventoryToSend = vInvWait;
        }
        if (!vInv.empty())
            pto->PushMessage("inv", vInv);


        //
        // Message: getdata
        //
        vector<CInv> vGetData;
        int64 nNow = GetTime() * 1000000;
        CTxDB txdb("r");
        while (!pto->mapAskFor.empty() && (*pto->mapAskFor.begin()).first <= nNow)
        {
            const CInv& inv = (*pto->mapAskFor.begin()).second;
            if (!AlreadyHave(txdb, inv))
            {
                if (fDebugNet)
                    printf("sending getdata: %s\n", inv.ToString().c_str());
                vGetData.push_back(inv);
                if (vGetData.size() >= 1000)
                {
                    pto->PushMessage("getdata", vGetData);
                    vGetData.clear();
                }
                mapAlreadyAskedFor[inv] = nNow;
            }
            pto->mapAskFor.erase(pto->mapAskFor.begin());
        }
        if (!vGetData.empty())
            pto->PushMessage("getdata", vGetData);

    }
    return true;
}














//////////////////////////////////////////////////////////////////////////////
//
// XcoinMiner
//

int static FormatHashBlocks(void* pbuffer, unsigned int len)
{
    unsigned char* pdata = (unsigned char*)pbuffer;
    unsigned int blocks = 1 + ((len + 8) / 64);
    unsigned char* pend = pdata + 64 * blocks;
    memset(pdata + len, 0, 64 * blocks - len);
    pdata[len] = 0x80;
    unsigned int bits = len * 8;
    pend[-1] = (bits >> 0) & 0xff;
    pend[-2] = (bits >> 8) & 0xff;
    pend[-3] = (bits >> 16) & 0xff;
    pend[-4] = (bits >> 24) & 0xff;
    return blocks;
}

static const unsigned int pSHA256InitState[8] =
{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

void SHA256Transform(void* pstate, void* pinput, const void* pinit)
{
    SHA256_CTX ctx;
    unsigned char data[64];

    SHA256_Init(&ctx);

    for (int i = 0; i < 16; i++)
        ((uint32_t*)data)[i] = ByteReverse(((uint32_t*)pinput)[i]);

    for (int i = 0; i < 8; i++)
        ctx.h[i] = ((uint32_t*)pinit)[i];

    SHA256_Update(&ctx, data, sizeof(data));
    for (int i = 0; i < 8; i++)
        ((uint32_t*)pstate)[i] = ctx.h[i];
}

//
// ScanHash scans nonces looking for a hash with at least some zero bits.
// It operates on big endian data.  Caller does the byte reversing.
// All input buffers are 16-byte aligned.  nNonce is usually preserved
// between calls, but periodically or if nNonce is 0xffff0000 or above,
// the block is rebuilt and nNonce starts over at zero.
//
unsigned int static ScanHash_CryptoPP(char* pmidstate, char* pdata, char* phash1, char* phash, unsigned int& nHashesDone)
{
    unsigned int& nNonce = *(unsigned int*)(pdata + 12);
    for (;;)
    {
        // Crypto++ SHA256
        // Hash pdata using pmidstate as the starting state into
        // pre-formatted buffer phash1, then hash phash1 into phash
        nNonce++;
        SHA256Transform(phash1, pdata, pmidstate);
        SHA256Transform(phash, phash1, pSHA256InitState);

        // Return the nonce if the hash has at least some zero bits,
        // caller will check if it has enough to reach the target
        if (((unsigned short*)phash)[14] == 0)
            return nNonce;

        // If nothing found after trying for a while, return -1
        if ((nNonce & 0xffff) == 0)
        {
            nHashesDone = 0xffff+1;
            return (unsigned int) -1;
        }
    }
}

// Some explaining would be appreciated
class COrphan
{
public:
    CTransaction* ptx;
    set<uint256> setDependsOn;
    double dPriority;
    double dFeePerKb;

    COrphan(CTransaction* ptxIn)
    {
        ptx = ptxIn;
        dPriority = dFeePerKb = 0;
    }

    void print() const
    {
        printf("COrphan(hash=%s, dPriority=%.1f, dFeePerKb=%.1f)\n",
               ptx->GetHash().ToString().substr(0,10).c_str(), dPriority, dFeePerKb);
        BOOST_FOREACH(uint256 hash, setDependsOn)
            printf("   setDependsOn %s\n", hash.ToString().substr(0,10).c_str());
    }
};


uint64 nLastBlockTx = 0;
uint64 nLastBlockSize = 0;

// We want to sort transactions by priority and fee, so:
typedef boost::tuple<double, double, CTransaction*> TxPriority;
class TxPriorityCompare
{
    bool byFee;
public:
    TxPriorityCompare(bool _byFee) : byFee(_byFee) { }
    bool operator()(const TxPriority& a, const TxPriority& b)
    {
        if (byFee)
        {
            if (a.get<1>() == b.get<1>())
                return a.get<0>() < b.get<0>();
            return a.get<1>() < b.get<1>();
        }
        else
        {
            if (a.get<0>() == b.get<0>())
                return a.get<1>() < b.get<1>();
            return a.get<0>() < b.get<0>();
        }
    }
};

CBlock* CreateNewBlock(CReserveKey& reservekey)
{
    // Create new block
    auto_ptr<CBlock> pblock(new CBlock());
    if (!pblock.get())
        return NULL;

    {
        LOCK2(cs_main, mempool.cs);
        CBlockIndex* pindexPrev = pindexBest;

        int nHeight = pindexPrev->nHeight + 1;

        std::map<CTxDestination, mpq> mapBudget;

        mpq nIDAmount = GetInitialDistributionAmount(nHeight);
        CBudget budgetID = GetInitialDistributionBudget(nHeight);
        ApplyBudget(nIDAmount, budgetID, mapBudget);

        mpq nPSAmount = GetPerpetualSubsidyAmount(nHeight);
        CBudget budgetPS = GetPerpetualSubsidyBudget(nHeight);
        ApplyBudget(nPSAmount, budgetPS, mapBudget);

        // To make sure that no transaction fee budgetary entries are dropped due
        // to truncation, we assume the largest theoretically possible transaction
        // fee, MAX_MONEY. Once the transactions for the new block have been
        // selected, we will go back and recreate the budget based on the actual
        // transaction fees.
        CBudget budgetTF = GetTransactionFeeBudget(nHeight);
        ApplyBudget(MPQ_MAX_MONEY, budgetTF, mapBudget);

        // Create coinbase tx
        CTransaction txNew;
        txNew.vin.resize(1);
        txNew.vin[0].prevout.SetNull();
        txNew.vout.resize(1+mapBudget.size());
        txNew.vout[0].scriptPubKey << reservekey.GetReservedKey() << OP_CHECKSIG;
        {
            std::map<CTxDestination, mpq>::iterator itr; int idx;
            for (itr = mapBudget.begin(), idx=1; itr != mapBudget.end(); ++itr, ++idx) {
                txNew.vout[idx].scriptPubKey.SetDestination(itr->first);
                txNew.vout[idx].SetInitialValue(RoundAbsolute(itr->second, ROUND_AWAY_FROM_ZERO));
            }
        }
        txNew.nRefHeight = nHeight;

        // Add our coinbase tx as first transaction
        pblock->vtx.push_back(txNew);

        // Largest block you're willing to create:
        unsigned int nBlockMaxSize = GetArg("-blockmaxsize", MAX_BLOCK_SIZE_GEN/2);
        // Limit to betweeen 1K and MAX_BLOCK_SIZE-1K for sanity:
        nBlockMaxSize = std::max((unsigned int)1000, std::min((unsigned int)(MAX_BLOCK_SIZE-1000), nBlockMaxSize));

        // How much of the block should be dedicated to high-priority transactions,
        // included regardless of the fees they pay
        unsigned int nBlockPrioritySize = GetArg("-blockprioritysize", 27000);
        nBlockPrioritySize = std::min(nBlockMaxSize, nBlockPrioritySize);

        // Minimum block size you want to create; block will be filled with free transactions
        // until there are no more or the block reaches this size:
        unsigned int nBlockMinSize = GetArg("-blockminsize", 0);
        nBlockMinSize = std::min(nBlockMaxSize, nBlockMinSize);

        // Fee-per-kilobyte amount considered the same as "free"
        // Be careful setting this: if you set it to zero then
        // a transaction spammer can cheaply fill blocks using
        // 1-satoshi-fee transactions. It should be set above the real
        // cost to you of processing a transaction.
        mpq nMinTxFee = MIN_TX_FEE;
        if (mapArgs.count("-mintxfee"))
            ParseMoney(mapArgs["-mintxfee"], nMinTxFee);

        // Collect memory pool transactions into the block
        mpq nFees = 0;

        CTxDB txdb("r");

        // Priority order to process transactions
        list<COrphan> vOrphan; // list memory doesn't move
        map<uint256, vector<COrphan*> > mapDependers;

        // This vector will be sorted into a priority queue:
        vector<TxPriority> vecPriority;
        vecPriority.reserve(mempool.mapTx.size());
        for (map<uint256, CTransaction>::iterator mi = mempool.mapTx.begin(); mi != mempool.mapTx.end(); ++mi)
        {
            CTransaction& tx = (*mi).second;
            if (tx.IsCoinBase() || !tx.IsFinal())
                continue;

            COrphan* porphan = NULL;
            double dPriority = 0;
            mpq nTotalIn = 0;
            bool fMissingInputs = false;
            BOOST_FOREACH(const CTxIn& txin, tx.vin)
            {
                // Read prev transaction
                CTransaction txPrev;
                CTxIndex txindex;
                if (!txPrev.ReadFromDisk(txdb, txin.prevout, txindex))
                {
                    // This should never happen; all transactions in the memory
                    // pool should connect to either transactions in the chain
                    // or other transactions in the memory pool.
                    if (!mempool.mapTx.count(txin.prevout.hash))
                    {
                        printf("ERROR: mempool transaction missing input\n");
                        if (fDebug) assert("mempool transaction missing input" == 0);
                        fMissingInputs = true;
                        if (porphan)
                            vOrphan.pop_back();
                        break;
                    }

                    // Has to wait for dependencies
                    if (!porphan)
                    {
                        // Use list for automatic deletion
                        vOrphan.push_back(COrphan(&tx));
                        porphan = &vOrphan.back();
                    }
                    mapDependers[txin.prevout.hash].push_back(porphan);
                    porphan->setDependsOn.insert(txin.prevout.hash);
                    const CTransaction &txPrevIn = mempool.mapTx[txin.prevout.hash];
                    nTotalIn += GetPresentValue(txPrevIn, txPrevIn.vout[txin.prevout.n], tx.nRefHeight);
                    continue;
                }

                int nConf = txindex.GetDepthInMainChain();

                mpq nValueIn = GetPresentValue(txPrev, txPrev.vout[txin.prevout.n], tx.nRefHeight);
                nTotalIn += nValueIn;

                dPriority += nTotalIn.get_d() * nConf;
            }
            if (fMissingInputs) continue;

            // Priority is sum(valuein * age) / txsize
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            dPriority /= nTxSize;

            // This is a more accurate fee-per-kilobyte than is used by the client code, because the
            // client code rounds up the size to the nearest 1K. That's good, because it gives an
            // incentive to create smaller transactions.
            mpq txValueOut = nTotalIn - tx.GetValueOut();
            double dFeePerKb = txValueOut.get_d() / (double(nTxSize)/1000.0);

            if (porphan)
            {
                porphan->dPriority = dPriority;
                porphan->dFeePerKb = dFeePerKb;
            }
            else
                vecPriority.push_back(TxPriority(dPriority, dFeePerKb, &(*mi).second));
        }

        // Collect transactions into block
        map<uint256, CTxIndex> mapTestPool;
        uint64 nBlockSize = 1000;
        uint64 nBlockTx = 0;
        int nBlockSigOps = 100;
        bool fSortedByFee = (nBlockPrioritySize <= 0);

        TxPriorityCompare comparer(fSortedByFee);
        std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);

        while (!vecPriority.empty())
        {
            // Take highest priority transaction off the priority queue:
            double dPriority = vecPriority.front().get<0>();
            double dFeePerKb = vecPriority.front().get<1>();
            CTransaction& tx = *(vecPriority.front().get<2>());

            std::pop_heap(vecPriority.begin(), vecPriority.end(), comparer);
            vecPriority.pop_back();

            // Invalid height
            if ( tx.nRefHeight > nHeight )
                continue;

            // Size limits
            unsigned int nTxSize = ::GetSerializeSize(tx, SER_NETWORK, PROTOCOL_VERSION);
            if (nBlockSize + nTxSize >= nBlockMaxSize)
                continue;

            // Legacy limits on sigOps:
            unsigned int nTxSigOps = tx.GetLegacySigOpCount();
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            // Skip free transactions if we're past the minimum block size:
            if (fSortedByFee && (dFeePerKb < nMinTxFee) && (nBlockSize + nTxSize >= nBlockMinSize))
                continue;

            // Prioritize by fee once past the priority size or we run out of high-priority
            // transactions:
            if (!fSortedByFee &&
                ((nBlockSize + nTxSize >= nBlockPrioritySize) || (dPriority < COIN * 144 / 250)))
            {
                fSortedByFee = true;
                comparer = TxPriorityCompare(fSortedByFee);
                std::make_heap(vecPriority.begin(), vecPriority.end(), comparer);
            }

            // Connecting shouldn't fail due to dependency on other memory pool transactions
            // because we're already processing them in order of dependency
            map<uint256, CTxIndex> mapTestPoolTmp(mapTestPool);
            MapPrevTx mapInputs;
            bool fInvalid;
            if (!tx.FetchInputs(txdb, mapTestPoolTmp, false, true, mapInputs, fInvalid))
                continue;

            mpq nNet = tx.GetValueIn(mapInputs) - tx.GetValueOut();
            mpq nTxFees = GetTimeAdjustedValue(nNet, nHeight-tx.nRefHeight);

            nTxSigOps += tx.GetP2SHSigOpCount(mapInputs);
            if (nBlockSigOps + nTxSigOps >= MAX_BLOCK_SIGOPS)
                continue;

            if (!tx.ConnectInputs(mapInputs, mapTestPoolTmp, CDiskTxPos(1,1,1), pindexPrev, false, true))
                continue;
            mapTestPoolTmp[tx.GetHash()] = CTxIndex(CDiskTxPos(1,1,1), tx.vout.size());
            swap(mapTestPool, mapTestPoolTmp);

            // Added
            pblock->vtx.push_back(tx);
            nBlockSize += nTxSize;
            ++nBlockTx;
            nBlockSigOps += nTxSigOps;
            nFees += nTxFees;

            if (fDebug && GetBoolArg("-printpriority"))
            {
                printf("priority %.1f feeperkb %.1f txid %s\n",
                       dPriority, dFeePerKb, tx.GetHash().ToString().c_str());
            }

            // Add transactions that depend on this one to the priority queue
            uint256 hash = tx.GetHash();
            if (mapDependers.count(hash))
            {
                BOOST_FOREACH(COrphan* porphan, mapDependers[hash])
                {
                    if (!porphan->setDependsOn.empty())
                    {
                        porphan->setDependsOn.erase(hash);
                        if (porphan->setDependsOn.empty())
                        {
                            vecPriority.push_back(TxPriority(porphan->dPriority, porphan->dFeePerKb, porphan->ptx));
                            std::push_heap(vecPriority.begin(), vecPriority.end(), comparer);
                        }
                    }
                }
            }
        }

        mapBudget.clear();
        ApplyBudget(nIDAmount, budgetID, mapBudget);
        ApplyBudget(nPSAmount, budgetPS, mapBudget);
        ApplyBudget(nFees, budgetTF, mapBudget);
        pblock->vtx[0].vout.resize(1+mapBudget.size());
        mpq nBudgetPaid = 0;
        {
            std::map<CTxDestination, mpq>::iterator itr; int idx;
            for (itr = mapBudget.begin(), idx=1; itr != mapBudget.end(); ++itr, ++idx) {
                txNew.vout[idx].scriptPubKey.SetDestination(itr->first);
                mpq qActual = RoundAbsolute(itr->second, ROUND_AWAY_FROM_ZERO);
                txNew.vout[idx].SetInitialValue(qActual);
                nBudgetPaid += qActual;
            }
        }

        nLastBlockTx = nBlockTx;
        nLastBlockSize = nBlockSize;
        printf("CreateNewBlock(): total size %"PRI64u"\n", nBlockSize);

        mpq nBlockReward = GetBlockValue(nHeight, nFees) - nBudgetPaid;
        pblock->vtx[0].vout[0].SetInitialValue(RoundAbsolute(nBlockReward, ROUND_TOWARDS_ZERO));

        // Fill in header
        pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
        pblock->UpdateTime(pindexPrev);
        pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock.get());
        pblock->nNonce         = 0;

        pblock->vtx[0].vin[0].scriptSig = CScript() << OP_0 << OP_0;
        CBlockIndex indexDummy(1, 1, *pblock);
        indexDummy.pprev = pindexPrev;
        indexDummy.nHeight = pindexPrev->nHeight + 1;
        if (!pblock->ConnectBlock(txdb, &indexDummy, true))
            throw std::runtime_error("CreateNewBlock() : ConnectBlock failed");
    }

    return pblock.release();
}


void IncrementExtraNonce(CBlock* pblock, CBlockIndex* pindexPrev, unsigned int& nExtraNonce)
{
    // Update nExtraNonce
    static uint256 hashPrevBlock;
    if (hashPrevBlock != pblock->hashPrevBlock)
    {
        nExtraNonce = 0;
        hashPrevBlock = pblock->hashPrevBlock;
    }
    ++nExtraNonce;
    unsigned int nHeight = pindexPrev->nHeight+1; // Height first in coinbase required for block.version=2
    pblock->vtx[0].vin[0].scriptSig = (CScript() << nHeight << CBigNum(nExtraNonce)) + COINBASE_FLAGS;
    assert(pblock->vtx[0].vin[0].scriptSig.size() <= 100);

    pblock->hashMerkleRoot = pblock->BuildMerkleTree();
}


void FormatHashBuffers(CBlock* pblock, char* pmidstate, char* pdata, char* phash1)
{
    //
    // Pre-build hash buffers
    //
    struct
    {
        struct unnamed2
        {
            int nVersion;
            uint256 hashPrevBlock;
            uint256 hashMerkleRoot;
            unsigned int nTime;
            unsigned int nBits;
            unsigned int nNonce;
        }
        block;
        unsigned char pchPadding0[64];
        uint256 hash1;
        unsigned char pchPadding1[64];
    }
    tmp;
    memset(&tmp, 0, sizeof(tmp));

    tmp.block.nVersion       = pblock->nVersion;
    tmp.block.hashPrevBlock  = pblock->hashPrevBlock;
    tmp.block.hashMerkleRoot = pblock->hashMerkleRoot;
    tmp.block.nTime          = pblock->nTime;
    tmp.block.nBits          = pblock->nBits;
    tmp.block.nNonce         = pblock->nNonce;

    FormatHashBlocks(&tmp.block, sizeof(tmp.block));
    FormatHashBlocks(&tmp.hash1, sizeof(tmp.hash1));

    // Byte swap all the input buffer
    for (unsigned int i = 0; i < sizeof(tmp)/4; i++)
        ((unsigned int*)&tmp)[i] = ByteReverse(((unsigned int*)&tmp)[i]);

    // Precalc the first half of the first hash, which stays constant
    SHA256Transform(pmidstate, &tmp.block, pSHA256InitState);

    memcpy(pdata, &tmp.block, 128);
    memcpy(phash1, &tmp.hash1, 64);
}


bool CheckWork(CBlock* pblock, CWallet& wallet, CReserveKey& reservekey)
{
    uint256 hash = pblock->GetHash();
    uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();

    if (hash > hashTarget)
        return false;

    //// debug print
    printf("XcoinMiner:\n");
    printf("proof-of-work found  \n  hash: %s  \ntarget: %s\n", hash.GetHex().c_str(), hashTarget.GetHex().c_str());
    pblock->print();
    printf("generated %s\n", FormatMoney(pblock->vtx[0].GetValueOut()).c_str());

    // Found a solution
    {
        LOCK(cs_main);
        if (pblock->hashPrevBlock != hashBestChain)
            return error("XcoinMiner : generated block is stale");

        // Remove key from key pool
        reservekey.KeepKey();

        // Track how many getdata requests this block gets
        {
            LOCK(wallet.cs_wallet);
            wallet.mapRequestCount[pblock->GetHash()] = 0;
        }

        // Process this block the same as if we had received it from another node
        if (!ProcessBlock(NULL, pblock))
            return error("XcoinMiner : ProcessBlock, block not accepted");
    }

    return true;
}

void static ThreadXcoinMiner(void* parg);

static bool fGenerateXcoins = false;
static bool fLimitProcessors = false;
static int nLimitProcessors = -1;

void static XcoinMiner(CWallet *pwallet)
{
    printf("XcoinMiner started\n");
    SetThreadPriority(THREAD_PRIORITY_LOWEST);

    // Make this thread recognisable as the mining thread
    RenameThread("x-miner");

    // Each thread has its own key and counter
    CReserveKey reservekey(pwallet);
    unsigned int nExtraNonce = 0;

    while (fGenerateXcoins)
    {
        if (fShutdown)
            return;
        while (vNodes.empty() || IsInitialBlockDownload())
        {
            Sleep(1000);
            if (fShutdown)
                return;
            if (!fGenerateXcoins)
                return;
        }


        //
        // Create new block
        //
        unsigned int nTransactionsUpdatedLast = nTransactionsUpdated;
        CBlockIndex* pindexPrev = pindexBest;

        auto_ptr<CBlock> pblock(CreateNewBlock(reservekey));
        if (!pblock.get())
            return;
        IncrementExtraNonce(pblock.get(), pindexPrev, nExtraNonce);

        printf("Running XcoinMiner with %"PRIszu" transactions in block (%u bytes)\n", pblock->vtx.size(),
               ::GetSerializeSize(*pblock, SER_NETWORK, PROTOCOL_VERSION));


        //
        // Pre-build hash buffers
        //
        char pmidstatebuf[32+16]; char* pmidstate = alignup<16>(pmidstatebuf);
        char pdatabuf[128+16];    char* pdata     = alignup<16>(pdatabuf);
        char phash1buf[64+16];    char* phash1    = alignup<16>(phash1buf);

        FormatHashBuffers(pblock.get(), pmidstate, pdata, phash1);

        unsigned int& nBlockTime = *(unsigned int*)(pdata + 64 + 4);
        unsigned int& nBlockBits = *(unsigned int*)(pdata + 64 + 8);
        unsigned int& nBlockNonce = *(unsigned int*)(pdata + 64 + 12);


        //
        // Search
        //
        int64 nStart = GetTime();
        uint256 hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
        uint256 hashbuf[2];
        uint256& hash = *alignup<16>(hashbuf);
        loop
        {
            unsigned int nHashesDone = 0;
            unsigned int nNonceFound;

            // Crypto++ SHA256
            nNonceFound = ScanHash_CryptoPP(pmidstate, pdata + 64, phash1,
                                            (char*)&hash, nHashesDone);

            // Check if something found
            if (nNonceFound != (unsigned int) -1)
            {
                for (unsigned int i = 0; i < sizeof(hash)/4; i++)
                    ((unsigned int*)&hash)[i] = ByteReverse(((unsigned int*)&hash)[i]);

                if (hash <= hashTarget)
                {
                    // Found a solution
                    pblock->nNonce = ByteReverse(nNonceFound);
                    assert(hash == pblock->GetHash());

                    SetThreadPriority(THREAD_PRIORITY_NORMAL);
                    CheckWork(pblock.get(), *pwalletMain, reservekey);
                    SetThreadPriority(THREAD_PRIORITY_LOWEST);
                    break;
                }
            }

            // Meter hashes/sec
            static int64 nHashCounter;
            if (nHPSTimerStart == 0)
            {
                nHPSTimerStart = GetTimeMillis();
                nHashCounter = 0;
            }
            else
                nHashCounter += nHashesDone;
            if (GetTimeMillis() - nHPSTimerStart > 4000)
            {
                static CCriticalSection cs;
                {
                    LOCK(cs);
                    if (GetTimeMillis() - nHPSTimerStart > 4000)
                    {
                        dHashesPerSec = 1000.0 * nHashCounter / (GetTimeMillis() - nHPSTimerStart);
                        nHPSTimerStart = GetTimeMillis();
                        nHashCounter = 0;
                        static int64 nLogTime;
                        if (GetTime() - nLogTime > 30 * 60)
                        {
                            nLogTime = GetTime();
                            printf("hashmeter %3d CPUs %6.0f khash/s\n", vnThreadsRunning[THREAD_MINER], dHashesPerSec/1000.0);
                        }
                    }
                }
            }

            // Check for stop or if block needs to be rebuilt
            if (fShutdown)
                return;
            if (!fGenerateXcoins)
                return;
            if (fLimitProcessors && vnThreadsRunning[THREAD_MINER] > nLimitProcessors)
                return;
            if (vNodes.empty())
                break;
            if (nBlockNonce >= 0xffff0000)
                break;
            if (nTransactionsUpdated != nTransactionsUpdatedLast && GetTime() - nStart > 60)
                break;
            if (pindexPrev != pindexBest)
                break;

            // Update nTime every few seconds
            pblock->UpdateTime(pindexPrev);
            nBlockTime = ByteReverse(pblock->nTime);
            if (fTestNet)
            {
                // Changing pblock->nTime can change work required on testnet:
                nBlockBits = ByteReverse(pblock->nBits);
                hashTarget = CBigNum().SetCompact(pblock->nBits).getuint256();
            }
        }
    }
}

void static ThreadXcoinMiner(void* parg)
{
    CWallet* pwallet = (CWallet*)parg;
    try
    {
        vnThreadsRunning[THREAD_MINER]++;
        XcoinMiner(pwallet);
        vnThreadsRunning[THREAD_MINER]--;
    }
    catch (std::exception& e) {
        vnThreadsRunning[THREAD_MINER]--;
        PrintException(&e, "ThreadXcoinMiner()");
    } catch (...) {
        vnThreadsRunning[THREAD_MINER]--;
        PrintException(NULL, "ThreadXcoinMiner()");
    }
    nHPSTimerStart = 0;
    if (vnThreadsRunning[THREAD_MINER] == 0)
        dHashesPerSec = 0;
    printf("ThreadXcoinMiner exiting, %d threads remaining\n", vnThreadsRunning[THREAD_MINER]);
}


void GenerateXcoins(bool fGenerate, CWallet* pwallet)
{
    fGenerateXcoins = fGenerate;
    nLimitProcessors = GetArg("-genproclimit", -1);
    if (nLimitProcessors == 0)
        fGenerateXcoins = false;
    fLimitProcessors = (nLimitProcessors != -1);

    if (fGenerate)
    {
        int nProcessors = boost::thread::hardware_concurrency();
        printf("%d processors\n", nProcessors);
        if (nProcessors < 1)
            nProcessors = 1;
        if (fLimitProcessors && nProcessors > nLimitProcessors)
            nProcessors = nLimitProcessors;
        int nAddThreads = nProcessors - vnThreadsRunning[THREAD_MINER];
        printf("Starting %d XcoinMiner threads\n", nAddThreads);
        for (int i = 0; i < nAddThreads; i++)
        {
            if (!NewThread(ThreadXcoinMiner, pwallet))
                printf("Error: NewThread(ThreadXcoinMiner) failed\n");
            Sleep(10);
        }
    }
}
