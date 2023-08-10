/**
 * @file threadedTesting.cpp
 * @author Kyle Quinlan (kyle.quinlan@colorado.edu)
 * @brief For rapid prototyping and testing of multithreaded control and acquisition code. See src/controlTests.cpp for single threaded development.
 * @version 0.1
 * @date 2023-06-30
 * 
 * @copyright Copyright (c) 2023
 * 
 */
#include "decs.hpp"

#define REFRESH_BAD_BINS (0)
#define REFRESH_BASELINE (0)

int main() {
    // Get ready for acquisition
    // Pumping parameters
    double xModeFreq = 4.985;  // GHz
    double yModeFreq = 7.455396; // GHz

    double diffFreq = yModeFreq - xModeFreq;
    double jpaFreq = xModeFreq * 2;

    double diffPower = 6.73; //dBm
    double jpaPower = 1.72; //dBm

    // Acquisition parameters
    double sampleRate = 32e6;
    double RBW = 100;
    int maxSpectraPerAcquisition = 250;

    double samplesPerSpectrum = sampleRate/RBW;
    double samplesPerAcquisition = samplesPerSpectrum*maxSpectraPerAcquisition;

    // Set up PSGs
    printAvailableResources();

    PSG psg1_Diff(27);
    PSG psg4_JPA(30);

    psg1_Diff.setFreq(diffFreq);
    psg1_Diff.setPow(diffPower);
    psg1_Diff.onOff(true);

    psg4_JPA.setFreq(jpaFreq);
    psg4_JPA.setPow(jpaPower);
    psg4_JPA.onOff(true);

    // Set up alazar card
    ATS alazarCard(1, 1);
    alazarCard.setAcquisitionParameters((U32)sampleRate, (U32)samplesPerAcquisition, maxSpectraPerAcquisition);
    std::cout << "Acquisition parameters set. Collecting " << std::to_string(alazarCard.acquisitionParams.buffersPerAcquisition) << " buffers." << std::endl;

    // Try to import an FFTW plan if available
    const char* wisdomFilePath = "fftw_wisdom.txt";
    if (fftw_import_wisdom_from_filename(wisdomFilePath) != 0) {
        std::cout << "Successfully imported FFTW wisdom from file." << std::endl;
    }
    else {
        std::cout << "Failed to import FFTW wisdom from file." << std::endl;
    }

    // Create an FFTW plan
    int N = (int)alazarCard.acquisitionParams.samplesPerBuffer;
    std::cout << "Creating plan for N = " << std::to_string(N) << std::endl;
    fftw_complex* fftwInput = reinterpret_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * N));
    fftw_complex* fftwOutput = reinterpret_cast<fftw_complex*>(fftw_malloc(sizeof(fftw_complex) * N));
    fftw_plan plan = fftw_plan_dft_1d(N, fftwInput, fftwOutput, FFTW_FORWARD, FFTW_MEASURE);
    std::cout << "Plan created!" << std::endl;

    // Create data processor
    DataProcessor dataProcessor;
    double cutoffFrequency = 10e3;
    int poleNumber = 3;
    double stopbandAttenuation = 15.0;
    dataProcessor.setFilterParams(alazarCard.acquisitionParams.sampleRate, poleNumber, cutoffFrequency, stopbandAttenuation);

    std::vector<double> freq(N);
    for (int i = 0; i < N; ++i) {
        freq[i] = (static_cast<double>(i)-static_cast<double>(N)/2)*sampleRate/N/1e6;
    }

    dataProcessor.loadSNR("../../../src/dataProcessing/visTheory.csv", "../../../src/dataProcessing/visTheoryFreqAxis.csv");

    // Try to import bad bins if available
    #if !REFRESH_BAD_BINS
    std::vector<double> badBins = readVector("badBins.csv");
    dataProcessor.badBins.reserve(badBins.size());

    std::transform(badBins.begin(), badBins.end(), std::back_inserter(dataProcessor.badBins), [](double d) { return static_cast<int>(d); }); // convert to int
    #endif

    #if !REFRESH_BASELINE
    dataProcessor.currentBaseline = readVector("baseline.csv");
    #endif

    // Create shared data structures
    SharedDataBasic sharedDataBasic;
    SharedDataProcessing sharedDataProc;
    SavedData savedData;
    SynchronizationFlags syncFlags;

    BayesFactors bayesFactors;

    sharedDataBasic.samplesPerBuffer = N;


    // Begin acquisition
    try{
        // Start the threads
        std::thread acquisitionThread(&ATS::AcquireDataMultithreadedContinuous, &alazarCard, std::ref(sharedDataBasic), std::ref(syncFlags));
        std::thread FFTThread(FFTThread, plan, N, std::ref(sharedDataBasic), std::ref(syncFlags));
        std::thread magnitudeThread(magnitudeThread, N, std::ref(sharedDataBasic), std::ref(sharedDataProc), std::ref(syncFlags), std::ref(dataProcessor));
        std::thread averagingThread(averagingThread, std::ref(sharedDataProc), std::ref(syncFlags), std::ref(dataProcessor), std::ref(yModeFreq));

        #if (!REFRESH_BASELINE && !REFRESH_BAD_BINS)
        std::thread processingThread(processingThread, std::ref(sharedDataProc), std::ref(savedData), std::ref(syncFlags), std::ref(dataProcessor), std::ref(bayesFactors));
        std::thread decisionMakingThread(decisionMakingThread, std::ref(sharedDataProc), std::ref(syncFlags));
        #endif

        #if SAVE_DATA
        std::thread savingThread(saveDataToBin, std::ref(sharedDataBasic), std::ref(syncFlags));
        #endif

        // Wait for the threads to finish
        acquisitionThread.join();
        FFTThread.join();
        magnitudeThread.join();
        averagingThread.join();

        #if (!REFRESH_BASELINE && !REFRESH_BAD_BINS)
        processingThread.join();
        decisionMakingThread.join();
        #endif

        #if SAVE_DATA
        savingThread.join();
        #endif
    }
    catch(const std::exception& e)
    {
        std::cerr << e.what() << '\n';
    }


    // Cleanup
    psg1_Diff.onOff(false);
    psg4_JPA.onOff(false);

    fftw_export_wisdom_to_filename(wisdomFilePath);
    std::cout << "FFTW wisdom saved to file." << std::endl;

    fftw_destroy_plan(plan);
    fftw_free(fftwInput);
    fftw_free(fftwOutput);

    reportPerformance();


    // Save the data
    std::vector<int> outliers = findOutliers(dataProcessor.runningAverage, 50, 4.5);

    saveVector(freq, "../../../plotting/threadTests/freq.csv");
    saveVector(outliers, "../../../plotting/threadTests/outliers.csv");

    dataProcessor.updateBaseline();
    saveVector(dataProcessor.currentBaseline, "../../../plotting/threadTests/baseline.csv");
    saveVector(dataProcessor.runningAverage, "../../../plotting/threadTests/runningAverage.csv");

    saveSpectrum(savedData.rawSpectra[0], "../../../plotting/threadTests/rawSpectrum.csv");

    saveSpectrum(savedData.processedSpectra[0], "../../../plotting/threadTests/processedSpectrum.csv");

    #if (!REFRESH_BASELINE && !REFRESH_BAD_BINS)
    CombinedSpectrum combinedSpectrum;
    
    for (Spectrum rescaledSpectrum : savedData.rescaledSpectra){
        dataProcessor.addRescaledToCombined(rescaledSpectrum, combinedSpectrum);
    }
    
    saveCombinedSpectrum(combinedSpectrum, "../../../plotting/threadTests/combinedSpectrum.csv");
    saveSpectrum(bayesFactors.exclusionLine, "../../../plotting/threadTests/exclusionLine.csv");
    #endif

    saveVector(dataProcessor.currentBaseline, "baseline.csv");

    #if REFRESH_BAD_BINS
    saveVector(outliers, "badBins.csv");
    #endif

    return 0;
}
