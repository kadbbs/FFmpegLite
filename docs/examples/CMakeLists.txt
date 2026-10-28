foreach(example memory_walkthrough file_transcode audio_memory encode_file)
  add_executable(fflite_doc_${example} ${example}.cpp)
  target_link_libraries(fflite_doc_${example} PRIVATE fflite::fflite)
endforeach()
