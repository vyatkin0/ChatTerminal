/*
$Id: ConsoleIo.h 22 2010-08-15 21:49:13Z avyatkin $

Declaration of global functions from the consoleio namespace, that works with the console or redirected input/output

Copyright (c) 2010 VyPRESS Research, LLC. All rights reserved.
For conditions of distribution and use, see copyright notice in ChatTerminal.h
*/

#pragma once
namespace consoleio
{
	// Flag is true if the input is from the console (not redirected)
	extern bool fConsoleIn;

	// Maximum size of the stack of executed commands, if 0 then stack is disabled
	extern unsigned int nCommandLinesStackSize;

	// Controls access to the print_bytes(), print_line(), console_read_input(), and other functions
	class PrintLinesMonitor
	{
	public:
		explicit PrintLinesMonitor()
		{
#ifdef CHATTERM_OS_WINDOWS
			EnterCriticalSection(&cs_);
#else
			pthread_mutex_lock( &m_ );
#endif// CHATTERM_OS_WINDOWS
		}

		~PrintLinesMonitor()
		{
#ifdef CHATTERM_OS_WINDOWS
			LeaveCriticalSection(&cs_);
#else
			pthread_mutex_unlock( &m_ );
#endif// CHATTERM_OS_WINDOWS
		}

	private:
		PrintLinesMonitor(const PrintLinesMonitor&) = delete;
		PrintLinesMonitor(ContainersMonitor&&) = delete;
		PrintLinesMonitor& operator=(const PrintLinesMonitor&) = delete;
		PrintLinesMonitor& operator=(ContainersMonitor&&) = delete;
		PrintLinesMonitor* operator&() = delete;

		static void Initialize()
		{
#ifdef CHATTERM_OS_WINDOWS
			InitializeCriticalSection(&cs_);
#else
			pthread_mutexattr_t attr;
			int err = pthread_mutexattr_init(&attr);
			err = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_PRIVATE);
			err = pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
			err = pthread_mutex_init(&m_, &attr);
#endif// CHATTERM_OS_WINDOWS
		}

		static void Delete()
		{
#ifdef CHATTERM_OS_WINDOWS
			DeleteCriticalSection(&cs_);
#else
			pthread_mutex_destroy(&m_);
#endif// CHATTERM_OS_WINDOWS
		}

#ifdef CHATTERM_OS_WINDOWS
		static CRITICAL_SECTION cs_;
#else
		static pthread_mutex_t m_;
#endif// CHATTERM_OS_WINDOWS

		/**
		Restore the console configuration, frees the console resources
		*/
		friend void console_finalize();
		
		/**
		Initializes the console, determines if input or output are redirected
		@return - 0 if successful, error code otherwise
		*/
		friend int console_initialize();
	};

#ifndef CHATTERM_OS_WINDOWS
	/**
	Restore the console configuration, frees the console resources
	*/
	void console_finalize();
	
	/**
	Initializes the console, determines if input or output are redirected
	@return - 0 if successful, error code otherwise
	*/
	int console_initialize();
#endif // CHATTERM_OS_WINDOWS

	/**
	Clears the console screen
	*/
	void console_clear_screen();

	/**
	Generates an empty input event for the console to unblock the application;
	It is used for notifications about network events
	*/
	void console_generate_event();

	/**
	Skips input during seconds seconds.
	Used for wait command
	@seconds - number of seconds to wait
	*/
	void console_skip_input(int seconds);

	/**
	Reads input. Returns an entered string when possible
	@strLine - reference to returned string
	@return - either length of the string, 0 if there is no string, or negative number in case of error
	*/
	int console_read_input(std::wstring& strLine);

	/**
	Prints formatted string to the output
	@color - text color
	@selected - if true the background-foreground inversion applied (text looks like selected)
	@format - format control
	@return - length of the formatted string
	*/
	int print_line(unsigned char color, bool selected, const wchar_t* format, ...);

	/**
	Prints formatted string to the output using default color and attributes
	@format -format control
	@return - length of the formatted string
	*/

	int print_line(const wchar_t* format, const std::wstring& strarg);
	int print_line(const wchar_t* format, ...);

	/**
	Prints formatted string to the output using default color and selection attributes
	@format -format control
	@return - length of the formatted string
	*/
	int print_line_selected(const wchar_t* format, ...);

	/**
	Prints bytes as hexadecimal numbers string
	@bytes - pointer to an array of bytes
	@len - size of the array
	*/
	void print_bytes(const unsigned char* bytes, unsigned int len);
};
