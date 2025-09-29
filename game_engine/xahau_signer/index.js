require('dotenv').config();
const express = require('express');
const cors = require('cors');
const fs = require('fs');
const path = require('path');
const { PinataSDK } = require('pinata');
const { Client, Wallet, xrpToDrops } = require('xahau');
const { encode, encodeForSigning } = require('xahau-binary-codec');
const { sign } = require('xahau-keypairs');
const { isValidClassicAddress } = require('xahau-address-codec');
const { derive } = require('xrpl-accountlib');

const app = express();
const PORT = process.env.PORT || 3001;

// Pinata configuration
let pinata = null;
const PINATA_JWT = process.env.PINATA_JWT;
const PINATA_GATEWAY = process.env.PINATA_GATEWAY;

if (PINATA_JWT) {
    pinata = new PinataSDK({
        pinataJwt: PINATA_JWT,
        pinataGateway: PINATA_GATEWAY || "sapphire-subsequent-walrus-610.mypinata.cloud"
    });
    console.log('✅ Pinata SDK initialized');
} else {
    console.warn('⚠️  Pinata credentials not found. Please configure .env file with PINATA_JWT');
}

// Asset paths
const ASSETS_DIR = path.join(process.cwd(), 'assets');
const IMAGES_DIR = path.join(ASSETS_DIR, 'images');
const METADATA_DIR = path.join(ASSETS_DIR, 'metadata');

// Middleware
app.use(cors());
app.use(express.json());

// Xahau client configuration
const XAHAU_NETWORK = process.env.XAHAU_NETWORK || 'wss://xahau-test.net';
const client = new Client(XAHAU_NETWORK);

// Store for maintaining client connection
let isConnected = false;

async function ensureConnection() {
    if (!isConnected) {
        await client.connect();
        isConnected = true;
        console.log('Connected to Xahau testnet');
    }
}

// =============================================================================
// ASSET MANAGEMENT AND IPFS HELPERS
// =============================================================================

/**
 * Find image file for an item (supports multiple extensions)
 */
function findImageFile(itemName) {
    const extensions = ['.jpg', '.jpeg', '.png', '.gif'];
    
    for (const ext of extensions) {
        const imagePath = path.join(IMAGES_DIR, itemName + ext);
        if (fs.existsSync(imagePath)) {
            return imagePath;
        }
    }
    return null;
}

/**
 * Load metadata template for an item
 */
function loadMetadataTemplate(itemName) {
    const metadataPath = path.join(METADATA_DIR, itemName + '.json');
    
    if (!fs.existsSync(metadataPath)) {
        return null;
    }
    
    try {
        const content = fs.readFileSync(metadataPath, 'utf8');
        return JSON.parse(content);
    } catch (error) {
        console.error(`Error loading metadata for ${itemName}:`, error);
        return null;
    }
}

/**
 * Upload file to IPFS via Pinata
 */
async function uploadToIPFS(filePath, fileName, isImage = true) {
    if (!pinata) {
        throw new Error('Pinata not configured. Please set PINATA_JWT environment variable.');
    }

    try {
        if (isImage) {
            // Upload image file with proper pinning configuration
            const fileBuffer = fs.readFileSync(filePath);
            const file = new File([fileBuffer], fileName, {
                type: 'image/jpeg' // or appropriate mime type
            });
            
            const options = {
                metadata: {
                    name: fileName,
                    description: `NFT image for ${fileName.split('_')[0]}`,
                    keyValues: {
                        itemType: 'nft_image',
                        itemName: fileName.split('_')[0]
                    }
                }
            };
            
            console.log(`[IPFS Upload] Uploading image file: ${fileName}`);
            const result = await pinata.upload.public.file(file, options);
            console.log(`[IPFS Upload] Image upload successful:`, JSON.stringify(result, null, 2));
            
            if (!result || !result.cid) {
                throw new Error(`Invalid response from Pinata: ${JSON.stringify(result)}`);
            }
            
            return `https://${PINATA_GATEWAY}/ipfs/${result.cid}`;
        } else {
            // Upload JSON metadata with proper pinning configuration  
            const metadata = JSON.parse(fs.readFileSync(filePath, 'utf8'));
            
            // Use the correct Pinata v1.8.1 API format for JSON uploads
            const jsonData = {
                content: metadata,
                name: fileName,
                lang: "json"
            };
            
            const options = {
                metadata: {
                    name: fileName,
                    description: `NFT metadata for ${fileName.split('_')[0]}`,
                    keyValues: {
                        itemType: 'nft_metadata',
                        itemName: fileName.split('_')[0]
                    }
                }
            };
            
            console.log(`[IPFS Upload] Uploading JSON metadata: ${fileName}`);
            const result = await pinata.upload.public.json(metadata);
            console.log(`[IPFS Upload] JSON upload successful:`, JSON.stringify(result, null, 2));
            
            if (!result || !result.cid) {
                throw new Error(`Invalid response from Pinata: ${JSON.stringify(result)}`);
            }
            
            return `https://${PINATA_GATEWAY}/ipfs/${result.cid}`;
        }
    } catch (error) {
        console.error(`[IPFS Upload] Error uploading ${fileName}:`, error);
        throw new Error(`IPFS upload failed for ${fileName}: ${error.message}`);
    }
}

/**
 * Process NFT item: upload image, update metadata, upload metadata
 */
async function processNFTItem(itemName) {
    console.log(`[Asset Processing] Processing item: ${itemName}`);
    
    // 1. Find image file
    const imagePath = findImageFile(itemName);
    if (!imagePath) {
        throw new Error(`Image file not found for item: ${itemName}`);
    }
    console.log(`[Asset Processing] Found image: ${path.basename(imagePath)}`);
    
    // 2. Load metadata template
    const metadata = loadMetadataTemplate(itemName);
    if (!metadata) {
        throw new Error(`Metadata template not found for item: ${itemName}`);
    }
    console.log(`[Asset Processing] Loaded metadata template for: ${itemName}`);
    
    // 3. Upload image to IPFS
    console.log(`[Asset Processing] Uploading image to IPFS...`);
    const imageUrl = await uploadToIPFS(imagePath, `${itemName}_image${path.extname(imagePath)}`, true);
    console.log(`[Asset Processing] Image uploaded: ${imageUrl}`);
    
    // 4. Update metadata with new image URL and mint timestamp
    metadata.image = imageUrl;
    
    // 5. Add unique mint timestamp to ensure unique CID
    metadata.mint_timestamp = Math.floor(Date.now() / 1000);
    metadata.mint_date = new Date().toISOString();
    metadata.unique_mint_id = `${itemName}_${Date.now()}_${Math.random().toString(36).substr(2, 9)}`;
    
    // 7. Create temporary metadata file and upload to IPFS
    const tempMetadataPath = path.join('/tmp', `${itemName}_metadata_${Date.now()}.json`);
    fs.writeFileSync(tempMetadataPath, JSON.stringify(metadata, null, 2));
    
    console.log(`[Asset Processing] Uploading metadata to IPFS...`);
    const metadataUrl = await uploadToIPFS(tempMetadataPath, `${itemName}_metadata.json`, false);
    console.log(`[Asset Processing] Metadata uploaded: ${metadataUrl}`);
    
    // 8. Clean up temporary file
    fs.unlinkSync(tempMetadataPath);
    
    return {
        itemName,
        imageUrl,
        metadataUrl,
        metadata
    };
}

// Health check endpoint
app.get('/health', (req, res) => {
    res.json({ 
        status: 'healthy', 
        connected: isConnected,
        timestamp: new Date().toISOString() 
    });
});

// Get account info
app.post('/account_info', async (req, res) => {
    try {
        const { account } = req.body;
        
        if (!account || !isValidClassicAddress(account)) {
            return res.status(400).json({ error: 'Valid account address required' });
        }

        await ensureConnection();
        
        const response = await client.request({
            command: 'account_info',
            account: account,
            ledger_index: 'validated'
        });

        res.json(response);
    } catch (error) {
        console.error('Account info error:', error);
        res.status(500).json({ error: error.message });
    }
});

// Sign transaction endpoint
app.post('/sign_transaction', async (req, res) => {
    try {
        const { transaction, secret } = req.body;
        
        if (!transaction || !secret) {
            return res.status(400).json({ error: 'Transaction and secret required' });
        }

        await ensureConnection();

        // Create wallet from secret with consistent algorithm
        const wallet = Wallet.fromSeed(secret, { algorithm: 'secp256k1' });
        
        // Prepare transaction for signing
        const prepared = await client.autofill(transaction);
        
        // Add NetworkID for Xahau testnet
        prepared.NetworkID = 21338;
        
        // Sign the transaction
        const signed = wallet.sign(prepared);
        
        res.json({
            tx_blob: signed.tx_blob,
            hash: signed.hash,
            transaction: prepared
        });
        
    } catch (error) {
        console.error('Transaction signing error:', error);
        res.status(500).json({ error: error.message });
    }
});

// Submit signed transaction (submit-only mode)
app.post('/submit', async (req, res) => {
    try {
        const { tx_blob } = req.body;
        
        if (!tx_blob) {
            return res.status(400).json({ error: 'tx_blob required' });
        }

        await ensureConnection();
        
        const response = await client.request({
            command: 'submit',
            tx_blob: tx_blob
        });

        res.json(response);
    } catch (error) {
        console.error('Transaction submission error:', error);
        res.status(500).json({ error: error.message });
    }
});

// Sign and submit transaction in one call
app.post('/sign_and_submit', async (req, res) => {
    try {
        const { transaction, secret } = req.body;
        
        if (!transaction || !secret) {
            return res.status(400).json({ error: 'Transaction and secret required' });
        }

        await ensureConnection();

        // Derive account from seed using xrpl-accountlib for consistent address derivation
        const account = derive.familySeed(secret, { algorithm: 'secp256k1' });
        console.log(`Derived address from seed: ${account.address}`);
        
        // Ensure transaction uses the correct account address
        const txWithCorrectAccount = { ...transaction, Account: account.address };
        
        // Create wallet from secret with consistent algorithm
        const wallet = Wallet.fromSeed(secret, { algorithm: 'secp256k1' });
        
        // Remove any existing LastLedgerSequence to let autofill set it fresh
        if (txWithCorrectAccount.LastLedgerSequence) {
            delete txWithCorrectAccount.LastLedgerSequence;
        }
        
        // Prepare transaction with autofill (let Xahau client handle LastLedgerSequence automatically)
        const prepared = await client.autofill(txWithCorrectAccount);
        
        // Add NetworkID for Xahau testnet
        prepared.NetworkID = 21338;
        
        console.log(`Final transaction:`, JSON.stringify(prepared, null, 2));
        
        // Sign the transaction
        const signed = wallet.sign(prepared);
        
        console.log(`Transaction signed successfully`);
        
        // Submit using tx_blob for better reliability
        const response = await client.request({
            command: 'submit',
            tx_blob: signed.tx_blob
        });

        console.log(`Transaction submitted:`, JSON.stringify(response.result || response, null, 2));
        res.json(response);
        
    } catch (error) {
        console.error('Sign and submit error:', error);
        res.status(500).json({ error: error.message });
    }
});

// Create URIToken Mint transaction
app.post('/create_uritoken_mint', async (req, res) => {
    try {
        const { 
            account, 
            uri, 
            destination, 
            amount, 
            flags = 1 
        } = req.body;
        
        if (!account || !uri) {
            return res.status(400).json({ error: 'Account and URI required' });
        }

        if (!isValidClassicAddress(account)) {
            return res.status(400).json({ error: 'Invalid account address' });
        }

        if (destination && !isValidClassicAddress(destination)) {
            return res.status(400).json({ error: 'Invalid destination address' });
        }

        await ensureConnection();

        // Build URITokenMint transaction
        const transaction = {
            TransactionType: 'URITokenMint',
            Account: account,
            URI: Buffer.from(uri, 'utf8').toString('hex').toUpperCase(),
            Flags: flags
        };

        // Add optional fields
        if (destination) {
            transaction.Destination = destination;
        }

        if (amount) {
            if (typeof amount === 'string' && !amount.includes('.')) {
                // XRP amount in drops
                transaction.Amount = amount;
            } else if (typeof amount === 'object') {
                // IOU amount
                transaction.Amount = amount;
            } else {
                // Convert XRP to drops
                transaction.Amount = xrpToDrops(amount.toString());
            }
        }

        res.json({ transaction });
        
    } catch (error) {
        console.error('URIToken creation error:', error);
        res.status(500).json({ error: error.message });
    }
});

// Get transaction by hash
app.post('/get_transaction', async (req, res) => {
    try {
        const { transaction } = req.body;
        
        if (!transaction) {
            return res.status(400).json({ error: 'Transaction hash required' });
        }

        await ensureConnection();
        
        const response = await client.request({
            command: 'tx',
            transaction: transaction
        });

        res.json(response);
    } catch (error) {
        console.error('Get transaction error:', error);
        res.status(500).json({ error: error.message });
    }
});

// Binary codec utilities
app.post('/encode_transaction', async (req, res) => {
    try {
        const { transaction } = req.body;
        
        if (!transaction) {
            return res.status(400).json({ error: 'Transaction required' });
        }

        const encoded = encode(transaction);
        res.json({ tx_blob: encoded });
        
    } catch (error) {
        console.error('Encoding error:', error);
        res.status(500).json({ error: error.message });
    }
});

app.post('/encode_for_signing', async (req, res) => {
    try {
        const { transaction } = req.body;
        
        if (!transaction) {
            return res.status(400).json({ error: 'Transaction required' });
        }

        const encoded = encodeForSigning(transaction);
        res.json({ signing_blob: encoded });
        
    } catch (error) {
        console.error('Encoding for signing error:', error);
        res.status(500).json({ error: error.message });
    }
});

// Get account address from seed
app.post('/get_account_address', async (req, res) => {
    try {
        const { secret } = req.body;
        
        if (!secret) {
            return res.status(400).json({ error: 'Secret required' });
        }

        // Derive account from seed using xrpl-accountlib for consistent address derivation
        const account = derive.familySeed(secret, { algorithm: 'secp256k1' });
        
        res.json({
            address: account.address,
            publicKey: account.keypair.publicKey,
            algorithm: 'secp256k1'
        });
        
    } catch (error) {
        console.error('Address derivation error:', error);
        res.status(500).json({ error: error.message });
    }
});

// Graceful shutdown
process.on('SIGINT', async () => {
    console.log('Shutting down gracefully...');
    if (isConnected) {
        await client.disconnect();
    }
    process.exit(0);
});

// =============================================================================
// NFT MINTING ENDPOINTS - Complete Pipeline
// =============================================================================

// Helper function to extract URITokenID from transaction metadata
function extractURITokenID(txResult) {
    try {
        if (txResult && txResult.meta && txResult.meta.AffectedNodes) {
            for (const node of txResult.meta.AffectedNodes) {
                if (node.CreatedNode && 
                    node.CreatedNode.LedgerEntryType === 'URIToken' &&
                    node.CreatedNode.NewFields &&
                    node.CreatedNode.NewFields.URITokenID) {
                    return node.CreatedNode.NewFields.URITokenID;
                }
            }
        }
    } catch (error) {
        console.error('Error extracting URITokenID:', error);
    }
    return null;
}

// Single NFT Minting - Complete Pipeline
app.post('/mint_nft', async (req, res) => {
    try {
        const { 
            account_seed, 
            metadata_uri, 
            item_name,
            destination = '',
            amount = '',
            flags = 1 
        } = req.body;
        
        // Validation
        if (!account_seed || !item_name) {
            return res.status(400).json({ 
                error: 'account_seed and item_name are required' 
            });
        }

        console.log(`[NFT Mint] Starting mint for item: ${item_name}`);
        
        let finalMetadataUri = metadata_uri;
        
        // If no metadata_uri provided, process assets and upload to IPFS
        if (!metadata_uri) {
            console.log(`[NFT Mint] No metadata URI provided, processing assets for: ${item_name}`);
            
            try {
                const processedAssets = await processNFTItem(item_name);
                finalMetadataUri = processedAssets.metadataUrl;
                console.log(`[NFT Mint] Assets processed successfully. Metadata URI: ${finalMetadataUri}`);
            } catch (assetError) {
                console.error(`[NFT Mint] Asset processing failed for ${item_name}:`, assetError);
                return res.status(400).json({
                    success: false,
                    item_name: item_name,
                    error_message: `Asset processing failed: ${assetError.message}`,
                    engine_result: 'tefBAD_REQUEST'
                });
            }
        } else {
            console.log(`[NFT Mint] Using provided metadata URI: ${finalMetadataUri}`);
        }

        await ensureConnection();

        // Derive account from seed using consistent algorithm
        const account = derive.familySeed(account_seed, { algorithm: 'secp256k1' });
        console.log(`[NFT Mint] Derived address: ${account.address}`);
        
        // Validate destination if provided
        if (destination && !isValidClassicAddress(destination)) {
            return res.status(400).json({ error: 'Invalid destination address' });
        }

        // Create wallet with consistent algorithm
        const wallet = Wallet.fromSeed(account_seed, { algorithm: 'secp256k1' });
        
        // Build URITokenMint transaction
        const transaction = {
            TransactionType: 'URITokenMint',
            Account: account.address,
            URI: Buffer.from(finalMetadataUri, 'utf8').toString('hex').toUpperCase(),
            Flags: flags
        };

        // Add optional fields
        if (destination) {
            transaction.Destination = destination;
        }

        if (amount) {
            if (typeof amount === 'string' && !amount.includes('.')) {
                transaction.Amount = amount;
            } else if (typeof amount === 'object') {
                transaction.Amount = amount;
            } else {
                transaction.Amount = xrpToDrops(amount.toString());
            }
        }

        console.log(`[NFT Mint] Created transaction for ${item_name}`);
        
        // Prepare transaction with autofill
        const prepared = await client.autofill(transaction);
        prepared.NetworkID = 21338;
        
        console.log(`[NFT Mint] Transaction prepared and autofilled`);
        
        // Sign the transaction
        const signed = wallet.sign(prepared);
        console.log(`[NFT Mint] Transaction signed successfully`);
        
        // Submit transaction
        const response = await client.request({
            command: 'submit',
            tx_blob: signed.tx_blob
        });

        console.log(`[NFT Mint] Transaction submitted for ${item_name}`);
        console.log(`[NFT Mint] Result:`, JSON.stringify(response.result || response, null, 2));

        // Parse response
        const result = response.result || response;
        const engineResult = result.engine_result || '';
        const transactionHash = result.hash || signed.hash;
        const success = engineResult === 'tesSUCCESS';
        
        // Extract URITokenID if successful
        let uritokenId = '';
        if (success && result.meta) {
            uritokenId = extractURITokenID(result) || `URITOKEN_${transactionHash.substring(0, 16)}`;
        }

        // Return standardized response
        const mintResult = {
            success: success,
            item_name: item_name,
            uritoken_id: uritokenId,
            transaction_hash: transactionHash,
            metadata_uri: finalMetadataUri,
            engine_result: engineResult,
            engine_result_code: result.engine_result_code || 0,
            validated: result.validated || false
        };

        if (!success) {
            mintResult.error_message = result.engine_result_message || `Transaction failed: ${engineResult}`;
            console.log(`[NFT Mint] ❌ Failed for ${item_name}: ${mintResult.error_message}`);
        } else {
            console.log(`[NFT Mint] ✅ Success for ${item_name}: ${transactionHash}`);
        }

        res.json(mintResult);
        
    } catch (error) {
        console.error('NFT minting error:', error);
        res.status(500).json({ 
            success: false,
            item_name: req.body.item_name || 'unknown',
            error_message: error.message,
            engine_result: 'tefFAILURE'
        });
    }
});

// Batch NFT Minting - Multiple NFTs
app.post('/mint_batch', async (req, res) => {
    try {
        const { account_seed, items } = req.body;
        
        // Validation
        if (!account_seed || !items || !Array.isArray(items) || items.length === 0) {
            return res.status(400).json({ 
                error: 'account_seed and items array are required' 
            });
        }

        console.log(`[Batch Mint] Starting batch mint for ${items.length} items`);

        await ensureConnection();

        // Derive account once for the batch
        const account = derive.familySeed(account_seed, { algorithm: 'secp256k1' });
        console.log(`[Batch Mint] Using account: ${account.address}`);

        const results = [];
        let successfulMints = 0;
        let failedMints = 0;

        // Process each item sequentially to avoid overwhelming the network
        for (let i = 0; i < items.length; i++) {
            const item = items[i];
            const { item_name, metadata_uri, destination = '', amount = '', flags = 1 } = item;

            if (!item_name) {
                results.push({
                    success: false,
                    item_name: item_name || `item_${i}`,
                    error_message: 'item_name is required',
                    engine_result: 'tefBAD_REQUEST'
                });
                failedMints++;
                continue;
            }

            let finalMetadataUri = metadata_uri;
            
            // If no metadata_uri provided, process assets and upload to IPFS
            if (!metadata_uri) {
                try {
                    console.log(`[Batch Mint] Processing assets for: ${item_name}`);
                    const processedAssets = await processNFTItem(item_name);
                    finalMetadataUri = processedAssets.metadataUrl;
                    console.log(`[Batch Mint] Assets processed for ${item_name}: ${finalMetadataUri}`);
                } catch (assetError) {
                    console.error(`[Batch Mint] Asset processing failed for ${item_name}:`, assetError);
                    results.push({
                        success: false,
                        item_name: item_name,
                        error_message: `Asset processing failed: ${assetError.message}`,
                        engine_result: 'tefBAD_REQUEST'
                    });
                    failedMints++;
                    continue;
                }
            }

            try {
                console.log(`[Batch Mint] Processing ${i + 1}/${items.length}: ${item_name}`);

                // Create wallet
                const wallet = Wallet.fromSeed(account_seed, { algorithm: 'secp256k1' });
                
                // Build transaction
                const transaction = {
                    TransactionType: 'URITokenMint',
                    Account: account.address,
                    URI: Buffer.from(finalMetadataUri, 'utf8').toString('hex').toUpperCase(),
                    Flags: flags
                };

                if (destination) transaction.Destination = destination;
                if (amount) {
                    if (typeof amount === 'string' && !amount.includes('.')) {
                        transaction.Amount = amount;
                    } else if (typeof amount === 'object') {
                        transaction.Amount = amount;
                    } else {
                        transaction.Amount = xrpToDrops(amount.toString());
                    }
                }

                // Prepare, sign and submit
                const prepared = await client.autofill(transaction);
                prepared.NetworkID = 21338;
                
                const signed = wallet.sign(prepared);
                
                const response = await client.request({
                    command: 'submit',
                    tx_blob: signed.tx_blob
                });

                // Parse result
                const result = response.result || response;
                const engineResult = result.engine_result || '';
                const transactionHash = result.hash || signed.hash;
                const success = engineResult === 'tesSUCCESS';
                
                let uritokenId = '';
                if (success && result.meta) {
                    uritokenId = extractURITokenID(result) || `URITOKEN_${transactionHash.substring(0, 16)}`;
                }

                const mintResult = {
                    success: success,
                    item_name: item_name,
                    uritoken_id: uritokenId,
                    transaction_hash: transactionHash,
                    metadata_uri: finalMetadataUri,
                    engine_result: engineResult,
                    engine_result_code: result.engine_result_code || 0,
                    validated: result.validated || false
                };

                if (!success) {
                    mintResult.error_message = result.engine_result_message || `Transaction failed: ${engineResult}`;
                    failedMints++;
                } else {
                    successfulMints++;
                }

                results.push(mintResult);
                
                // Small delay between transactions to avoid rate limiting
                if (i < items.length - 1) {
                    await new Promise(resolve => setTimeout(resolve, 500));
                }

            } catch (itemError) {
                console.error(`[Batch Mint] Error processing ${item_name}:`, itemError);
                results.push({
                    success: false,
                    item_name: item_name,
                    error_message: itemError.message,
                    engine_result: 'tefFAILURE'
                });
                failedMints++;
            }
        }

        // Return batch summary
        const batchResult = {
            success: failedMints === 0,
            total_requested: items.length,
            successful_mints: successfulMints,
            failed_mints: failedMints,
            batch_timestamp: Math.floor(Date.now() / 1000),
            results: results
        };

        console.log(`[Batch Mint] Completed: ${successfulMints} success, ${failedMints} failed`);
        res.json(batchResult);

    } catch (error) {
        console.error('Batch minting error:', error);
        res.status(500).json({ 
            success: false,
            error_message: error.message,
            total_requested: req.body.items ? req.body.items.length : 0,
            successful_mints: 0,
            failed_mints: req.body.items ? req.body.items.length : 0,
            results: []
        });
    }
});

app.listen(PORT, () => {
    // Set environment variable for the signing service URL
    const signingServiceUrl = `http://localhost:${PORT}`;
    process.env.SIGNING_SERVICE_URL = signingServiceUrl;
    
    console.log(`🚀 Xahau Transaction Signer service running on port ${PORT}`);
    console.log(`🌐 Service URL: ${signingServiceUrl}`);
    console.log(`🔗 Xahau Network: ${XAHAU_NETWORK}`);
    console.log(`📁 Assets Directory: ${ASSETS_DIR}`);
    console.log(`💾 Environment variable set: SIGNING_SERVICE_URL=${signingServiceUrl}`);
    console.log(`\n📋 Available Endpoints:`);
    console.log(`   Health check: ${signingServiceUrl}/health`);
    console.log(`   NFT Minting: ${signingServiceUrl}/mint_nft`);
    console.log(`   Batch Minting: ${signingServiceUrl}/mint_batch`);
    
    // Check environment configuration
    console.log(`\n🔧 Configuration Status:`);
    console.log(`   Pinata IPFS: ${pinata ? '✅ Configured' : '❌ Not configured'}`);
    console.log(`   Asset Files: ${fs.existsSync(IMAGES_DIR) && fs.existsSync(METADATA_DIR) ? '✅ Found' : '❌ Missing'}`);
    
    if (!pinata) {
        console.log(`\n⚠️  To enable IPFS uploads, create a .env file with Pinata credentials:`);
        console.log(`   cp .env.example .env`);
        console.log(`   # Edit .env with your Pinata API keys`);
    }
    
    // Write environment file for other processes
    const envContent = `SIGNING_SERVICE_URL=${signingServiceUrl}\n`;
    fs.writeFileSync('.env.signing', envContent);
    console.log(`\n📝 Environment file created: .env.signing`);
});
