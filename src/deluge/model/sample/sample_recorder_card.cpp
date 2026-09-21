/*
 * Copyright © 2016-2023 Synthstrom Audible Limited
 * Copyright © 2026 Deluge Community Firmware contributors
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "model/output.h"
#include "model/sample/sample.h"
#include "model/sample/sample_recorder.h"
#include "model/song/song.h"
#include "processing/engines/audio_engine.h"
#include "processing/stem_export/stem_export.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/storage_manager.h"
#include "util/functions.h"

// Returns error if one occurred just now - not if one was already noted before
Error SampleRecorder::cardRoutine() {
	if (loopIOActive_ || loopPreparing_)
		return Error::NONE;

	// A backing underrun is terminal for this take. Do not let a later write
	// failure replace it while waiting for the view to acknowledge the error.
	if (isLoopOverdub() && captureOutputBackingMissing)
		abort();

	// If aborted, delete the file.
	if (status == RecorderStatus::ABORTED) {

aborted:
		return cleanupAbortedRecording();
	}

	if (status >= RecorderStatus::COMPLETE) {
		return Error::NONE;
	}

	Error error = Error::NONE;

	if (!hadCardError) {

		// If file not created yet, do that
		if (filePathCreated.isEmpty()) {

			error = StorageManager::initSD();
			if (error != Error::NONE) {
				goto gotError;
			}

			// Check there's space on the card
			error = StorageManager::checkSpaceOnCard();
			if (error != Error::NONE) {
				goto gotError;
			}

			String filePath;
			String tempFilePathForRecording;

			// Note: we couldn't pass the actual Sample pointer into this function, cos the Sample might get destructed
			// during the card access! (Though probably not anymore right?)
			// Recording could finish or abort during this!
			if (stemExport.processStarted) {
				error = stemExport.getUnusedStemRecordingFilePath(&filePath, folderID);
			}
			else {
				const char* name = "Audio";
				if (mode < AUDIO_INPUT_CHANNEL_FIRST_INTERNAL_OPTION && !AudioEngine::lineInPluggedIn) {
					if (AudioEngine::micPluggedIn) {
						name = "ExtMic";
					}
					else {
						name = "IntMic";
					}
				}
				else if (mode == AudioInputChannel::SPECIFIC_OUTPUT) {
					if (outputRecordingFrom) {
						name = outputRecordingFrom->name.get();
					}
				}
				else {
					name = inputChannelToString(mode);
				}
				error = audioFileManager.getUnusedAudioRecordingFilePath(filePath, &tempFilePathForRecording, folderID,
				                                                         &audioFileNumber, name, &currentSong->name);
			}
			if (status == RecorderStatus::ABORTED) {
				goto aborted; // In case aborted during
			}
			if (error != Error::NONE) {
				goto gotError;
			}

			bool mayOverwrite = true;

			// Now store our own copy of the actually (possibly temp) filename
			if (!tempFilePathForRecording.isEmpty()) {
				filePathCreated.set(&tempFilePathForRecording); // Can't fail!
			}
			else {
				filePathCreated.set(&filePath); // Can't fail!
				mayOverwrite = false;
			}

			// Recording could finish or abort during this!
			auto created = StorageManager::createFile(filePathCreated.get(), mayOverwrite);
			if (!created) {
				error = created.error();
				filePathCreated.clear();
				goto gotError;
			}
			else {
				this->file = created.value();
			}

			if (status == RecorderStatus::ABORTED) {
				goto aborted; // In case aborted during
			}

			// Ok, the Sample still exists.
			sample->filePath.set(&filePath);                                 // Can't fail!
			sample->tempFilePathForRecording.set(&tempFilePathForRecording); // Can't fail!

			error = audioFileManager.audioFiles.insertElement(sample);
			if (error != Error::NONE) {
				goto gotError;
			}

			haveAddedSampleToArray = true;
		}

		if (isLoopOverdub()) {
			error = serviceLoopWrites();
			if (error != Error::NONE) {
				if (terminalError == Error::NONE)
					terminalError = error;
				// I/O failures are classified by the writer; allocation/backing
				// failures must not invent a filesystem error. End capture now.
				abort();
			}
			return error;
		}

		// Might want to write just one cluster
		if (firstUnwrittenClusterIndex < currentRecordClusterIndex) {
			error = writeOneCompletedCluster();

			if (error != Error::NONE) {
gotError:
				if (terminalError == Error::NONE) {
					terminalError = error;
				}
				hadCardError = true;
			}

			else {
				// If more clusters still to write, come back later to do them
				if (true || firstUnwrittenClusterIndex < currentRecordClusterIndex) {
					goto allDoneForNow;
				}
			}
		}
	}

	// If we've actually finished recording...
	if (status == RecorderStatus::FINISHED_CAPTURING_BUT_STILL_WRITING) {
		if (!hadCardError) {
			error = finalizeRecordedFile();
			if (error != Error::NONE) {
				if (terminalError == Error::NONE) {
					terminalError = error;
				}
				hadCardError = true;
			}
		}

		if (reachedMaxFileSize) {
			if (autoDeleteWhenDone) {
				abort();
			}
			else {
				status = RecorderStatus::COMPLETE;
			}
			error = Error::MAX_FILE_SIZE_REACHED;
		}
		else {
			status = autoDeleteWhenDone ? RecorderStatus::AWAITING_DELETION : RecorderStatus::COMPLETE;
		}
	}

allDoneForNow:
	return error;
}
