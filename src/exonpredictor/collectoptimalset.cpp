#include <string>
#include <vector>
#include <sstream>
#include <sys/time.h>

#include "LocalParameters.h"
#include "Matcher.h"
#include "DBReader.h"
#include "DBWriter.h"
#include "Debug.h"
#include "Util.h"
#include "MathUtil.h"
#include <limits>
#include <cstdint>
#include <queue>
#include "itoa.h"

#ifdef OPENMP
#include <omp.h>
#endif

const int PLUS = 1;
const int MINUS = -1;

const int MINIMAL_INTRON_LENGTH = 15;
int const MAX_AA_OVERLAP = 10;

// simple consts, to be changed in the future:
int const CONST_LEGAL_OVERLAP_PENALTY = -5;
int const GAP_OPEN_PENALTY = -1;
int const GAP_EXTEND_PENALTY = -1;

struct potentialExon {
	// constructor
	potentialExon(int iMMSeqs2Key, int iAlnScore, int iContigStart, int iContigEnd, int iStrand, int iProteinMatchStart, int iProteinMatchEnd, int iProteinLen, float iPotentialExonSequenceIdentity, double iPotentialExonEval) :
		MMSeqs2Key(iMMSeqs2Key), alnScore(iAlnScore), contigStart(iContigStart), contigEnd(iContigEnd), strand(iStrand), proteinMatchStart(iProteinMatchStart), proteinMatchEnd(iProteinMatchEnd) {
            // update the result_t object:
            float proteinCover = float(proteinMatchEnd - proteinMatchStart + 1) / iProteinLen;
            int potentialExonLengthInNucleotides = iContigEnd - iContigStart + 1;
            int alignemntLength = potentialExonLengthInNucleotides / 3;
            if ((potentialExonLengthInNucleotides % 3 != 0) || ((alignemntLength * 3) != potentialExonLengthInNucleotides)) {
                std::string potentialExonStr = potentialExonToStr();
                Debug(Debug::ERROR) << "ERROR: seems like the coordiantes do not dictate a legal length for a codon segment. " << potentialExonStr << "\n";
                EXIT(EXIT_FAILURE);
            }

            // since the search was swapped, the "db" is the potentialExon and the "q" is the protein
            potentialExonAlignemntRes.score      = iAlnScore;
            potentialExonAlignemntRes.qcov       = proteinCover;
            potentialExonAlignemntRes.dbcov      = 1.0;
            potentialExonAlignemntRes.seqId      = iPotentialExonSequenceIdentity;
            potentialExonAlignemntRes.eval       = iPotentialExonEval;
            potentialExonAlignemntRes.alnLength  = alignemntLength;

            potentialExonAlignemntRes.qStartPos  = iProteinMatchStart;
            potentialExonAlignemntRes.qEndPos    = iProteinMatchEnd;
            potentialExonAlignemntRes.qLen       = iProteinLen;

            potentialExonAlignemntRes.dbKey      = iMMSeqs2Key;
            potentialExonAlignemntRes.dbStartPos = iContigStart;
            potentialExonAlignemntRes.dbEndPos   = iContigEnd;
            potentialExonAlignemntRes.dbLen      = potentialExonLengthInNucleotides;
            
            potentialExonAlignemntRes.backtrace  = "";
	}
	
	// information extracted from MMSeqs2 local alignment
    int MMSeqs2Key;
    int alnScore;
    // contig start and end refer to the first (and last) nucleotides to participate in the alignment
    // the coordinates are with respect to the contig start (5', plus strand) and are negative
    // in case of the minus strand. This way, in both strands, start < end.
	int contigStart;
	int contigEnd;
	int strand;
    int proteinMatchStart;
	int proteinMatchEnd;

    // will assist in printing to result file:
    Matcher::result_t potentialExonAlignemntRes;

    // to allow sorting a vector of potentialExon by their start on the contig
    static bool comparePotentialExons (const potentialExon & aPotentialExon, const potentialExon & anotherPotentialExon){
        if(aPotentialExon.contigStart < anotherPotentialExon.contigStart)
            return true;
        if(aPotentialExon.contigStart > anotherPotentialExon.contigStart)
            return false;
        // the following lines will break even cases in a consistent way
        if(aPotentialExon.contigEnd < anotherPotentialExon.contigEnd)
            return true;
        if(aPotentialExon.contigEnd > anotherPotentialExon.contigEnd)
            return false;
        // if this line is reached, it is the same potentialExon (same start & same end)...
        return false;
    }

    std::string potentialExonToStr() {
        std::stringstream ss;
        ss << "MMSeqs2Key: " << MMSeqs2Key << ", alnScore: " << alnScore << ", contigStart: " << contigStart << ", contigEnd: " << contigEnd << ", proteinMatchStart: " << proteinMatchStart << ", proteinMatchEnd: " << proteinMatchEnd;
        return (ss.str());
    }
};

struct dpMatrixRow {
    // constructor
    dpMatrixRow(size_t iPrevPotentialExonId, int iPathScore) : prevPotentialExonId(iPrevPotentialExonId), pathScore(iPathScore) {

    }

    // the prevPotentialExonId refers to the row Id (i.e., the sorted order)
    size_t prevPotentialExonId;
    int pathScore;
};

bool isPairCompatible(const potentialExon & firstPotentialExonOnContig, const potentialExon & secondPotentialExonOnContig) {
	// it is assumed firstPotentialExonOnContig comes before secondPotentialExonOnContig on contig, i.e.:
    // firstPotentialExonOnContig.contigStart <= secondPotentialExonOnContig.contigStart
    // because negative coordinates are used for the minus strand, the logic works

    // check same strand:
	if (firstPotentialExonOnContig.strand != secondPotentialExonOnContig.strand) {
		return false;
	}
    
    // check the first one does not contain the second one:
	if (secondPotentialExonOnContig.contigEnd < firstPotentialExonOnContig.contigEnd) {
		return false;
	}

	// check gap/overlap on contig:
	int diffOnContig = secondPotentialExonOnContig.contigStart - firstPotentialExonOnContig.contigEnd - 1;
	if (diffOnContig < MINIMAL_INTRON_LENGTH) {
		return false;
	}

	// check gap/overlap on target:
	int diffAAs = secondPotentialExonOnContig.proteinMatchStart - firstPotentialExonOnContig.proteinMatchEnd - 1;
	if (diffAAs < -(MAX_AA_OVERLAP)) {
		return false;
	}

    // check contig order is as target order:
    if (secondPotentialExonOnContig.proteinMatchStart < firstPotentialExonOnContig.proteinMatchStart) {
        return false;
    }

	return true;
}

int getPenaltyForProtCoords(const potentialExon & prevPotentialExon, const potentialExon & currPotentialExon) {
	// this function is called on a compatible pair
    int diffAAs = currPotentialExon.proteinMatchStart - prevPotentialExon.proteinMatchEnd - 1;
	if (diffAAs < 0) {
		// legal overlap that should be penalized:
		return CONST_LEGAL_OVERLAP_PENALTY;
	}
	else if (diffAAs <= 1) {
		// no penalty for missing up to one AA in between exons:
		return 0;
	}
	else {
        // penalize for missing protein fragment:
        // currently GAP_OPEN_PENALTY = GAP_EXTEND_PENALTY so this is linear penalty
		int penalty = GAP_OPEN_PENALTY + GAP_EXTEND_PENALTY * (diffAAs - 1);
		return penalty;
	}

	// never reached:
	return 1;
}

void findoptimalsetbydp(std::vector<potentialExon> & potentialExonCandidates, std::vector<potentialExon> & optimalExonSet) {   
    size_t numPotentialExonCandidates = potentialExonCandidates.size();
    if (numPotentialExonCandidates == 0) {
        // nothing to do here!
        return;
    }

    // sort vector by start on contig:
    std::stable_sort(potentialExonCandidates.begin(), potentialExonCandidates.end(), potentialExon::comparePotentialExons);

    // prevIdsAndScoresBestPath will hold the DP computation results
    // Each row i represents a potentialExon. They are sorted according to the start on the contig.
    // Each row i is of the struct dpMatrixRow, which works as follows:
	// prevPotentialExonId keeps the id j such that j is the previous potentialExon
	// on the best path ending with the potentialExon i. It will allow for the trace back.
	// pathScore contains the score itself
    std::vector<dpMatrixRow> prevIdsAndScoresBestPath;
    prevIdsAndScoresBestPath.reserve(numPotentialExonCandidates);
	// initialize:
	for (size_t id = 0; id < numPotentialExonCandidates; ++id) {
		prevIdsAndScoresBestPath.emplace_back(dpMatrixRow(id, potentialExonCandidates[id].alnScore));
	}

    int bestPathScore = 0;
	int lastPotentialExonInBestPath = 0;
	// dynamic programming to fill in the matrix, go over all rows - previous values have been computed:
    for (size_t currPotentialExonId = 0; currPotentialExonId < numPotentialExonCandidates; ++currPotentialExonId) {
        for (size_t prevPotentialExonId = 0; prevPotentialExonId < currPotentialExonId; ++prevPotentialExonId) {
            if (isPairCompatible(potentialExonCandidates[prevPotentialExonId], potentialExonCandidates[currPotentialExonId])) {
                int bestScorePathPrevIsLast = prevIdsAndScoresBestPath[prevPotentialExonId].pathScore;
				int costOfPrevToCurrTransition = getPenaltyForProtCoords(potentialExonCandidates[prevPotentialExonId], potentialExonCandidates[currPotentialExonId]);
				int currScoreWithPrev = bestScorePathPrevIsLast + costOfPrevToCurrTransition + potentialExonCandidates[currPotentialExonId].alnScore;

                // update row of currPotentialExon in case of improvement:
				if (currScoreWithPrev > prevIdsAndScoresBestPath[currPotentialExonId].pathScore) {
					prevIdsAndScoresBestPath[currPotentialExonId].prevPotentialExonId = prevPotentialExonId;
					prevIdsAndScoresBestPath[currPotentialExonId].pathScore = currScoreWithPrev;
				}
            }
        }

        // update the global max in case of improvement:
		if (prevIdsAndScoresBestPath[currPotentialExonId].pathScore > bestPathScore) {
			lastPotentialExonInBestPath = currPotentialExonId;
			bestPathScore = prevIdsAndScoresBestPath[currPotentialExonId].pathScore;
		}
    }

    // trace back (for now, just with print):
    int currExonId = lastPotentialExonInBestPath;
	while (prevIdsAndScoresBestPath[currExonId].prevPotentialExonId != currExonId) {
        optimalExonSet.emplace_back(potentialExonCandidates[currExonId]);
		currExonId = prevIdsAndScoresBestPath[currExonId].prevPotentialExonId;
	}
    // include in the optimal set
    optimalExonSet.emplace_back(potentialExonCandidates[currExonId]);
}

size_t fillBufferWithExonsResults (std::vector<potentialExon> & optimalExonSet, char * allExonResultsBuffer) {

    int numExonsInOptimalSet = optimalExonSet.size();
    size_t numCharsWritten = 0;
    // go over vector in reverse order (the last exon is in place 0 in the vector)
    for (int i = (numExonsInOptimalSet - 1); i >= 0; --i) {
        size_t exonResLen = Matcher::resultToBuffer(&allExonResultsBuffer[numCharsWritten], optimalExonSet[i].potentialExonAlignemntRes, false);
        numCharsWritten += exonResLen;
    }
    return (numCharsWritten);
}

size_t fillBufferWithMapInfo (char * mapBuffer, int proteinID, int contigID, int strand, double evalue) {
    char * basePos = mapBuffer;
    char * tmpBuff = Itoa::u32toa_sse2(static_cast<uint32_t>(proteinID), mapBuffer);
    *(tmpBuff-1) = '\t';
    tmpBuff = Itoa::u32toa_sse2(static_cast<uint32_t>(contigID), tmpBuff);
    *(tmpBuff-1) = '\t';
    tmpBuff = Itoa::i32toa_sse2(static_cast<uint32_t>(strand), tmpBuff);
    *(tmpBuff-1) = '\t';
    tmpBuff += sprintf(tmpBuff,"%.3E",evalue);
    tmpBuff++;
    *(tmpBuff-1) = '\n';
    *(tmpBuff) = '\0';

    return (tmpBuff - basePos);
}

int collectoptimalset(int argn, const char **argv, const Command& command) {
    LocalParameters& par = LocalParameters::getLocalInstance();
    par.parseParameters(argn, argv, command, 2, true, true);

    DBReader<unsigned int> resultReader(par.db1.c_str(), par.db1Index.c_str());
    resultReader.open(DBReader<unsigned int>::LINEAR_ACCCESS);

    // this key is joint to several threads so will be increamented in crtical:
    size_t globalMapKey = 0; 
    
    std::string dbProteinContigStrandMap = par.db2 + "_protein_contig_strand_map";
    std::string dbProteinContigStrandMapIndex = par.db2 + "_protein_contig_strand_map.index";
    DBWriter mapWriter(dbProteinContigStrandMap.c_str(), dbProteinContigStrandMapIndex.c_str(), par.threads);
    mapWriter.open();

    std::string dbOptimalExons = par.db2 + "_optimal_exon_sets";
    std::string dbOptimalExonsIndex = par.db2 + "_optimal_exon_sets.index";
    DBWriter optimalExonsWriter(dbOptimalExons.c_str(), dbOptimalExonsIndex.c_str(), par.threads);
    optimalExonsWriter.open();

    // analyze each entry of the result DB, this is a swapped DB
    // so the original targets play the role of queries...
    // i.e., the results are from protein --> potentialExon
#pragma omp parallel
    {
        unsigned int thread_idx = 0;
#ifdef OPENMP
        thread_idx = static_cast<unsigned int>(omp_get_thread_num());
#endif
        std::vector<potentialExon> plusStrandPotentialExons;
        plusStrandPotentialExons.reserve(10000);
        std::vector<potentialExon> minusStrandPotentialExons;
        minusStrandPotentialExons.reserve(10000);
        std::vector<potentialExon> plusStrandOptimalExonSet;
        plusStrandOptimalExonSet.reserve(100);
        std::vector<potentialExon> minusStrandOptimalExonSet;
        minusStrandOptimalExonSet.reserve(100);

        char *entry[255];          
        
#pragma omp for schedule(dynamic, 100)
        for (size_t id = 0; id < resultReader.getSize(); id++) {
            Debug::printProgress(id);

            unsigned int proteinID = resultReader.getDbKey(id);

            char *results = resultReader.getData(id);
            
            int currContigId = -1;
            bool isFirstIteration = true;

            char exonsResultsBuffer[10000];
            char mapBuffer[1000]; 
            
            while (*results != '\0') {
                const size_t columns = Util::getWordsOfLine(results, entry, 255);

                // the structure of entry is a concatentaion of two alignemnts
                // the first alignemnt is with respect to protein<-->potentialExon
                // the second alignemnt is with respect to potentialExon<-->contig
                // the number of columns can be 20 or 22 (depending on the "-a" option)
                size_t firstColumnOfSecondAlignemnt = 10;
                if (columns == 22) {
                    firstColumnOfSecondAlignemnt = 11;
                }

                unsigned int potentialExonMMSeqs2Key = Util::fast_atoi<int>(entry[0]);
                int potentialExonToProteinAlnScore = Util::fast_atoi<int>(entry[1]);
                double potentialExonSequenceIdentity = atof(entry[2]);
                double potentialExonEvalue = atof(entry[3]);
                int proteinMatchStart =  Util::fast_atoi<int>(entry[4]);
                int proteinMatchEnd = Util::fast_atoi<int>(entry[5]);
                int proteinLen = Util::fast_atoi<int>(entry[6]);
                int potentialExonMatchStart = Util::fast_atoi<int>(entry[7]);
                int potentialExonMatchEnd = Util::fast_atoi<int>(entry[8]);

                // take relavant info from potentialExon<-->contig alignment:
                int potentialExonContigId = Util::fast_atoi<int>(entry[firstColumnOfSecondAlignemnt]);
                int potentialExonContigStartBeforeTrim = Util::fast_atoi<int>(entry[firstColumnOfSecondAlignemnt + 7]);
                int potentialExonContigEndBeforeTrim = Util::fast_atoi<int>(entry[firstColumnOfSecondAlignemnt + 8]);

                // "trim" the potentialExon, i.e., adjust start and end on the contig:
                int potentialExonContigStart;
                int potentialExonContigEnd;
                int potentialExonStrand;

                // plus strand:
                if (potentialExonContigStartBeforeTrim < potentialExonContigEndBeforeTrim) {
                    potentialExonContigStart = potentialExonContigStartBeforeTrim + (potentialExonMatchStart * 3);
                    potentialExonContigEnd = potentialExonContigStartBeforeTrim + (potentialExonMatchEnd * 3) + 2;
                    potentialExonStrand = PLUS;
                }
                // mimus strand:
                else {
                    // multiplying by minus allows carrying out same logic as with plus strand:
                    potentialExonContigStart = -1 * (potentialExonContigStartBeforeTrim - (potentialExonMatchStart * 3));
                    potentialExonContigEnd = -1 * (potentialExonContigStartBeforeTrim - (potentialExonMatchEnd * 3) - 2);
                    potentialExonStrand = MINUS;
                }

                if (isFirstIteration) {
                    currContigId = potentialExonContigId;
                    isFirstIteration = false;
                }

                if (potentialExonContigId != currContigId) {
                    if (potentialExonContigId < currContigId) {
                        Debug(Debug::ERROR) << "ERROR: the contigs are assumed to be sorted in increasing order. This doesn't seem to be the case.\n";
                        EXIT(EXIT_FAILURE);
                    }
                    // sort + dynamic programming to find the optimals set:
                    findoptimalsetbydp(plusStrandPotentialExons, plusStrandOptimalExonSet);
                    findoptimalsetbydp(minusStrandPotentialExons, minusStrandOptimalExonSet);
                    
                    // write optimal sets to result file:
                    if (plusStrandOptimalExonSet.size() > 0) {
                        size_t mapKey;
                        // take the value from the global keys counter - only a single thread can access it at a given time:
                        #pragma omp critical (getUniqueKeyForCombination)  
                        {  
                            mapKey = globalMapKey;
                            globalMapKey++;
                        }
                        size_t mapCombinationLen = fillBufferWithMapInfo(mapBuffer, proteinID, currContigId, PLUS, 0.01);
                        mapWriter.writeData(mapBuffer, mapCombinationLen, mapKey, thread_idx);

                        size_t exonsResultsLen = fillBufferWithExonsResults (plusStrandOptimalExonSet, exonsResultsBuffer); 
                        optimalExonsWriter.writeData(exonsResultsBuffer, exonsResultsLen, mapKey, thread_idx);
                    }
                    if (minusStrandOptimalExonSet.size() > 0) {
                        size_t mapKey;
                        // take the value from the global keys counter - only a single thread can access it at a given time:
                        #pragma omp critical (getUniqueKeyForCombination)  
                        {  
                            mapKey = globalMapKey;
                            globalMapKey++;
                        }
                        size_t mapCombinationLen = fillBufferWithMapInfo(mapBuffer, proteinID, currContigId, MINUS, 0.01);
                        mapWriter.writeData(mapBuffer, mapCombinationLen, mapKey, thread_idx);

                        size_t exonsResultsLen = fillBufferWithExonsResults (minusStrandOptimalExonSet, exonsResultsBuffer); 
                        optimalExonsWriter.writeData(exonsResultsBuffer, exonsResultsLen, mapKey, thread_idx);
                    }

                    // empty vectors:
                    plusStrandPotentialExons.clear();
                    minusStrandPotentialExons.clear();
                    plusStrandOptimalExonSet.clear();
                    minusStrandOptimalExonSet.clear();
                    currContigId = potentialExonContigId;
                }
                
                // push current potentialExon struct to vector:
                if (potentialExonStrand == PLUS) {
                    plusStrandPotentialExons.emplace_back(potentialExonMMSeqs2Key, potentialExonToProteinAlnScore, potentialExonContigStart, potentialExonContigEnd, potentialExonStrand, proteinMatchStart, proteinMatchEnd, proteinLen, potentialExonSequenceIdentity, potentialExonEvalue);
                } else {
                    minusStrandPotentialExons.emplace_back(potentialExonMMSeqs2Key, potentialExonToProteinAlnScore, potentialExonContigStart, potentialExonContigEnd, potentialExonStrand, proteinMatchStart, proteinMatchEnd, proteinLen, potentialExonSequenceIdentity, potentialExonEvalue);
                }

                results = Util::skipLine(results);
            }

            // one last time - required for the matches of the last contig against the protein
            // sort + dynamic programming to find the optimals set:
            findoptimalsetbydp(plusStrandPotentialExons, plusStrandOptimalExonSet);
            findoptimalsetbydp(minusStrandPotentialExons, minusStrandOptimalExonSet);
            
            // write optimal sets to result file:
            if (plusStrandOptimalExonSet.size() > 0) {
                size_t mapKey;
                // take the value from the global keys counter - only a single thread can access it at a given time:
                #pragma omp critical (getUniqueKeyForCombination)  
                {  
                    mapKey = globalMapKey;
                    globalMapKey++;
                }
                size_t mapCombinationLen = fillBufferWithMapInfo(mapBuffer, proteinID, currContigId, PLUS, 0.01);
                mapWriter.writeData(mapBuffer, mapCombinationLen, mapKey, thread_idx);

                size_t exonsResultsLen = fillBufferWithExonsResults (plusStrandOptimalExonSet, exonsResultsBuffer); 
                optimalExonsWriter.writeData(exonsResultsBuffer, exonsResultsLen, mapKey, thread_idx);
            }
            if (minusStrandOptimalExonSet.size() > 0) {
                size_t mapKey;
                // take the value from the global keys counter - only a single thread can access it at a given time:
                #pragma omp critical (getUniqueKeyForCombination)  
                {  
                    mapKey = globalMapKey;
                    globalMapKey++;
                }
                size_t mapCombinationLen = fillBufferWithMapInfo(mapBuffer, proteinID, currContigId, MINUS, 0.01);
                mapWriter.writeData(mapBuffer, mapCombinationLen, mapKey, thread_idx);

                size_t exonsResultsLen = fillBufferWithExonsResults (minusStrandOptimalExonSet, exonsResultsBuffer); 
                optimalExonsWriter.writeData(exonsResultsBuffer, exonsResultsLen, mapKey, thread_idx);
            }

            // empty vectors (between protein records):
            plusStrandPotentialExons.clear();
            minusStrandPotentialExons.clear();
            plusStrandOptimalExonSet.clear();
            minusStrandOptimalExonSet.clear();
        }
    }

    // cleanup
    resultReader.close();

    mapWriter.close();
    optimalExonsWriter.close();
    
    Debug(Debug::INFO) << "\nDone.\n";

    return EXIT_SUCCESS;
}


