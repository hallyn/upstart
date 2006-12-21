/* libupstart
 *
 * test_wire.c - test suite for upstart/wire.c
 *
 * Copyright © 2006 Canonical Ltd.
 * Author: Scott James Remnant <scott@ubuntu.com>.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA
 */

#include <nih/test.h>

#include <sys/types.h>
#include <sys/socket.h>

#include <nih/macros.h>

#include <upstart/control.h>
#include <upstart/wire.h>


void
test_write_int (void)
{
	struct iovec iovec;
	char         buf[14];
	int          ret;

	TEST_FUNCTION ("upstart_write_int");
	iovec.iov_base = buf;
	iovec.iov_len = 0;

	/* Check that we can write an integer into an empty iovec that has
	 * room; the integer should show up in network byte order at the
	 * start of the buffer, and the length of the buffer should be
	 * increased.
	 */
	TEST_FEATURE ("with space in empty buffer");
	ret = upstart_write_int (&iovec, sizeof (buf), 42);

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 4);
	TEST_EQ_MEM (iovec.iov_base, "\0\0\0\x2a", 4);


	/* Check that we can write an integer into an iovec that already has
	 * some thing in it, it should be appended and the buffer increased
	 * in length to include both.
	 */
	TEST_FEATURE ("with space in used buffer");
	ret = upstart_write_int (&iovec, sizeof (buf), 1234567);

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 8);
	TEST_EQ_MEM (iovec.iov_base, "\0\0\0\x2a\0\x12\xd6\x87", 8);


	/* Check that we can place a negative number into the iovec. */
	TEST_FEATURE ("with negative number");
	ret = upstart_write_int (&iovec, sizeof (buf), -42);

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 12);
	TEST_EQ_MEM (iovec.iov_base + 8, "\xff\xff\xff\xd6", 4);


	/* Check that -1 is returned if there is not enough space in the
	 * buffer for the integer, and that the length is incremented past
	 * the size to indicate an invalid message.
	 */
	TEST_FEATURE ("with insufficient space in buffer");
	ret = upstart_write_int (&iovec, sizeof (buf), 100);

	TEST_LT (ret, 0);
	TEST_EQ (iovec.iov_len, 16);
}

void
test_read_int (void)
{
	struct iovec iovec;
	char         buf[14];
	size_t       pos;
	int          ret, value;

	TEST_FUNCTION ("upstart_read_int");
	iovec.iov_base = buf;
	iovec.iov_len = 14;
	memcpy (iovec.iov_base, "\0\0\0\x2a\0\x12\xd6\x87\xff\xff\xff\xd6\0\0",
		14);
	pos = 0;

	/* Check that we can read an integer from the start of an iovec;
	 * the integer should be returned in host byte order from the start
	 * of the buffer, and the pos variable should be incremented past it.
	 */
	TEST_FEATURE ("with integer at start of buffer");
	ret = upstart_read_int (&iovec, &pos, &value);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 4);
	TEST_EQ (value, 42);


	/* Check that we can read an integer from a position inside the
	 * iovec.  The pos variable should be incremented, not set.
	 */
	TEST_FEATURE ("with integer inside buffer");
	ret = upstart_read_int (&iovec, &pos, &value);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 8);
	TEST_EQ (value, 1234567);


	/* Check that we can read a negative number from an iovec. */
	TEST_FEATURE ("with negative number");
	ret = upstart_read_int (&iovec, &pos, &value);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 12);
	TEST_EQ (value, -42);


	/* Check that -1 is returned if there is not enough space in the
	 * buffer for an integer, and that the pos is incremented past
	 * the size to indicate an invalid message.  value should be
	 * unchanged.
	 */
	TEST_FEATURE ("with insufficient space in buffer");
	ret = upstart_read_int (&iovec, &pos, &value);

	TEST_LT (ret, 0);
	TEST_EQ (pos, 16);
	TEST_EQ (value, -42);
}


void
test_write_unsigned (void)
{
	struct iovec iovec;
	char         buf[14];
	int          ret;

	TEST_FUNCTION ("upstart_write_unsigned");
	iovec.iov_base = buf;
	iovec.iov_len = 0;

	/* Check that we can write an integer into an empty iovec that has
	 * room; the integer should show up in network byte order at the
	 * start of the buffer, and the length of the buffer should be
	 * increased.
	 */
	TEST_FEATURE ("with space in empty buffer");
	ret = upstart_write_unsigned (&iovec, sizeof (buf), 42);

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 4);
	TEST_EQ_MEM (iovec.iov_base, "\0\0\0\x2a", 4);


	/* Check that we can write an integer into an iovec that already has
	 * some thing in it, it should be appended and the buffer increased
	 * in length to include both.
	 */
	TEST_FEATURE ("with space in used buffer");
	ret = upstart_write_unsigned (&iovec, sizeof (buf), 1234567);

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 8);
	TEST_EQ_MEM (iovec.iov_base, "\0\0\0\x2a\0\x12\xd6\x87", 8);


	/* Check that we can write a very large number into the iovec. */
	TEST_FEATURE ("with very large number");
	ret = upstart_write_unsigned (&iovec, sizeof (buf), 0xfedcba98);

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 12);
	TEST_EQ_MEM (iovec.iov_base + 8, "\xfe\xdc\xba\x98", 4);


	/* Check that -1 is returned if there is not enough space in the
	 * buffer for the integer, and that the length is incremented past
	 * the size to indicate an invalid message.
	 */
	TEST_FEATURE ("with insufficient space in buffer");
	ret = upstart_write_unsigned (&iovec, sizeof (buf), 100);

	TEST_LT (ret, 0);
	TEST_EQ (iovec.iov_len, 16);
}

void
test_read_unsigned (void)
{
	struct iovec iovec;
	char         buf[14];
	size_t       pos;
	unsigned int value;
	int          ret;

	TEST_FUNCTION ("upstart_read_unsigned");
	iovec.iov_base = buf;
	iovec.iov_len = 14;
	memcpy (iovec.iov_base, "\0\0\0\x2a\0\x12\xd6\x87\xfe\xdc\xba\x98\0\0",
		14);
	pos = 0;

	/* Check that we can read an integer from the start of an iovec;
	 * the integer should be returned in host byte order from the start
	 * of the buffer, and the pos variable should be incremented past it.
	 */
	TEST_FEATURE ("with integer at start of buffer");
	ret = upstart_read_unsigned (&iovec, &pos, &value);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 4);
	TEST_EQ_U (value, 42);


	/* Check that we can read an integer from a position inside the
	 * iovec.  The pos variable should be incremented, not set.
	 */
	TEST_FEATURE ("with integer inside buffer");
	ret = upstart_read_unsigned (&iovec, &pos, &value);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 8);
	TEST_EQ_U (value, 1234567);


	/* Check that we can read a very large number from an iovec. */
	TEST_FEATURE ("with very large number");
	ret = upstart_read_unsigned (&iovec, &pos, &value);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 12);
	TEST_EQ_U (value, 0xfedcba98);


	/* Check that -1 is returned if there is not enough space in the
	 * buffer for an integer, and that the pos is incremented past
	 * the size to indicate an invalid message.  value should be
	 * unchanged.
	 */
	TEST_FEATURE ("with insufficient space in buffer");
	ret = upstart_read_unsigned (&iovec, &pos, &value);

	TEST_LT (ret, 0);
	TEST_EQ (pos, 16);
	TEST_EQ_U (value, 0xfedcba98);
}


void
test_write_string (void)
{
	struct iovec iovec;
	char         buf[34];
	int          ret;

	TEST_FUNCTION ("upstart_write_string");
	iovec.iov_base = buf;
	iovec.iov_len = 0;

	/* Check that we can write a string into an empty iovec that has
	 * room; the string should show up with the length in network byte
	 * order at the start of the buffer, followed by the string bytes.
	 * The length of the buffer should be increased.
	 */
	TEST_FEATURE ("with space in empty buffer");
	ret = upstart_write_string (&iovec, sizeof (buf), "hello");

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 9);
	TEST_EQ_MEM (iovec.iov_base, "\0\0\0\x05hello", 9);


	/* Check that we can write a string into an iovec that already has
	 * some thing in it, it should be appended and the buffer increased
	 * in length to include both.
	 */
	TEST_FEATURE ("with space in used buffer");
	ret = upstart_write_string (&iovec, sizeof (buf), "goodbye");

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 20);
	TEST_EQ_MEM (iovec.iov_base, "\0\0\0\x05hello\0\0\0\x07goodbye", 20);


	/* Check that we can write the empty string into the iovec. */
	TEST_FEATURE ("with empty string");
	ret = upstart_write_string (&iovec, sizeof (buf), "");

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 24);
	TEST_EQ_MEM (iovec.iov_base + 20, "\0\0\0\0", 4);


	/* Check that we can write NULL into the iovec. */
	TEST_FEATURE ("with NULL string");
	ret = upstart_write_string (&iovec, sizeof (buf), NULL);

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 28);
	TEST_EQ_MEM (iovec.iov_base + 24, "\xff\xff\xff\xff", 4);


	/* Check that -1 is returned if there is not enough space in the
	 * buffer for the length of the string, and that the length is
	 * incremented past the size to indicate an invalid message.
	 */
	TEST_FEATURE ("with insufficient space in buffer for length");
	ret = upstart_write_string (&iovec, sizeof (buf) - 4, "test");

	TEST_LT (ret, 0);
	TEST_EQ (iovec.iov_len, 32);


	/* Check that -1 is returned if there is enough space in the buffer
	 * for the length of the string, but not the string, and that the
	 * length is incremented past the size to indicate an invalid
	 * message.
	 */
	TEST_FEATURE ("with insufficient space for string");
	iovec.iov_len = 28;
	ret = upstart_write_string (&iovec, sizeof (buf), "test");

	TEST_LT (ret, 0);
	TEST_EQ (iovec.iov_len, 36);
}

void
test_read_string (void)
{
	struct iovec iovec;
	char         buf[34], *value;
	size_t       pos;
	int          ret;

	TEST_FUNCTION ("upstart_read_int");
	iovec.iov_base = buf;
	iovec.iov_len = 34;
	memcpy (iovec.iov_base, ("\0\0\0\x05hello\0\0\0\x07goodbye"
				 "\0\0\0\0\xff\xff\xff\xff"
				 "\0\0\0\x04te"), 34);
	pos = 0;

	/* Check that we can read a string from the start of an iovec;
	 * the string should be allocated with nih_alloc, copied from the
	 * start of the buffer and include a NULL terminator.  The pos
	 * variable should be incremented past it.
	 */
	TEST_FEATURE ("with string at start of buffer");
	ret = upstart_read_string (&iovec, &pos, NULL, &value);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 9);
	TEST_ALLOC_SIZE (value, 6);
	TEST_EQ (value[5], '\0');
	TEST_EQ_STR (value, "hello");

	nih_free (value);


	/* Check that we can read a string from a position inside the
	 * iovec.  The pos variable should be incremented, not set.
	 */
	TEST_FEATURE ("with string inside buffer");
	ret = upstart_read_string (&iovec, &pos, NULL, &value);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 20);
	TEST_ALLOC_SIZE (value, 8);
	TEST_EQ (value[7], '\0');
	TEST_EQ_STR (value, "goodbye");

	nih_free (value);


	/* Check that we can read the empty string from the iovec. */
	TEST_FEATURE ("with empty string in buffer");
	ret = upstart_read_string (&iovec, &pos, NULL, &value);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 24);
	TEST_ALLOC_SIZE (value, 1);
	TEST_EQ (value[0], '\0');

	nih_free (value);


	/* Check that we can read NULL from the iovec. */
	TEST_FEATURE ("with NULL string in buffer");
	ret = upstart_read_string (&iovec, &pos, NULL, &value);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 28);
	TEST_EQ_P (value, NULL);


	/* Check that -1 is returned if there is enough space in the buffer
	 * for the length of the string, but not the string, and that
	 * pos is incremented past the size to indicate an invalid
	 * message.
	 */
	TEST_FEATURE ("with insufficient space for string");
	ret = upstart_read_string (&iovec, &pos, NULL, &value);

	TEST_LT (ret, 0);
	TEST_EQ (pos, 36);
	TEST_EQ_P (value, NULL);


	/* Check that -1 is returned if there is not enough space in the
	 * buffer for the length of the string, and that pos is
	 * incremented past the size to indicate an invalid message.
	 */
	TEST_FEATURE ("with insufficient space in buffer for length");
	pos = 28;
	iovec.iov_len = sizeof (buf) - 4;
	ret = upstart_read_string (&iovec, &pos, NULL, &value);

	TEST_LT (ret, 0);
	TEST_EQ (pos, 32);
	TEST_EQ_P (value, NULL);
}


void
test_write_header (void)
{
	struct iovec iovec;
	char         buf[34];
	int          ret;

	TEST_FUNCTION ("upstart_write_header");
	iovec.iov_base = buf;
	iovec.iov_len = 0;

	/* Check that we can write a header into an empty iovec that has
	 * room; the magic string should be written at the start of the
	 * buffer, followed by the message type in network byte order.
	 * The length of the buffer should be increased.
	 */
	TEST_FEATURE ("with space in empty buffer");
	ret = upstart_write_header (&iovec, sizeof (buf), UPSTART_NO_OP);

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 12);
	TEST_EQ_MEM (iovec.iov_base, "upstart\n\0\0\0\0", 12);


	/* Check that we can write a header into an iovec that already has
	 * something in it, it should be appended and the buffer increased
	 * in length to include both.
	 */
	TEST_FEATURE ("with space in used buffer");
	ret = upstart_write_header (&iovec, sizeof (buf), UPSTART_NO_OP);

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 24);
	TEST_EQ_MEM (iovec.iov_base, "upstart\n\0\0\0\0upstart\n\0\0\0\0", 24);


	/* Check that -1 is returned if there is not enough space in the
	 * buffer for the magic string, and that the length is
	 * incremented past the size to indicate an invalid message.
	 */
	TEST_FEATURE ("with insufficient space in buffer for magic");
	ret = upstart_write_header (&iovec, sizeof (buf) - 4, UPSTART_NO_OP);

	TEST_LT (ret, 0);
	TEST_EQ (iovec.iov_len, 32);


	/* Check that -1 is returned if there is enough space in the buffer
	 * for the magic string, but not the message type, and that the
	 * length is incremented past the size to indicate an invalid
	 * message.
	 */
	TEST_FEATURE ("with insufficient space for message type");
	iovec.iov_len = 28;
	ret = upstart_write_header (&iovec, sizeof (buf), UPSTART_NO_OP);

	TEST_LT (ret, 0);
	TEST_EQ (iovec.iov_len, 36);
}

void
test_read_header (void)
{
	struct iovec   iovec;
	char           buf[34];
	size_t         pos;
	UpstartMsgType value;
	int            ret;

	TEST_FUNCTION ("upstart_read_header");
	iovec.iov_base = buf;
	iovec.iov_len = 34;
	memcpy (iovec.iov_base,
		"upstart\n\0\0\0\0upstart\n\0\0\0\0upstart\n\0\0", 34);
	pos = 0;

	/* Check that we can read a header from the start of an iovec,
	 * and have the message type stored in value.  The pos variable
	 * should be incremented past the header.
	 */
	TEST_FEATURE ("with header at start of buffer");
	ret = upstart_read_header (&iovec, &pos, &value);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 12);
	TEST_EQ (value, UPSTART_NO_OP);


	/* Check that we can read a header from a position inside the
	 * iovec.  The pos variable should be incremented, not set.
	 */
	TEST_FEATURE ("with string inside buffer");
	ret = upstart_read_header (&iovec, &pos, &value);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 24);
	TEST_EQ (value, UPSTART_NO_OP);


	/* Check that -1 is returned if there is enough space in the buffer
	 * for the magic string, but not the message type, and that
	 * pos is incremented past the size to indicate an invalid
	 * message.
	 */
	TEST_FEATURE ("with insufficient space for message type");
	value = -1;
	ret = upstart_read_header (&iovec, &pos, &value);

	TEST_LT (ret, 0);
	TEST_EQ (pos, 36);
	TEST_EQ (value, -1);


	/* Check that -1 is returned if there is not enough space in the
	 * buffer for the magic string, and that pos is incremented past
	 * the size to indicate an invalid message.
	 */
	TEST_FEATURE ("with insufficient space in buffer for magic");
	pos = 24;
	iovec.iov_len = sizeof (buf) - 4;
	ret = upstart_read_header (&iovec, &pos, &value);

	TEST_LT (ret, 0);
	TEST_EQ (pos, 32);
	TEST_EQ (value, -1);
}


void
test_write_pack (void)
{
	struct iovec iovec;
	char         buf[46];
	int          ret;

	TEST_FUNCTION ("upstart_write_pack");
	iovec.iov_base = buf;
	iovec.iov_len = 0;

	/* Check that we can write a series of different values in a single
	 * function call, resulting in them being placed at the start of the
	 * iovec in order.
	 */
	TEST_FEATURE ("with empty buffer");
	ret = upstart_write_pack (&iovec, sizeof (buf), "iusi",
				  100, 0x98765432, "string value", -42);

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 28);
	TEST_EQ_MEM (iovec.iov_base, ("\0\0\0\x64\x98\x76\x54\x32"
				      "\0\0\0\x0cstring value"
				      "\xff\xff\xff\xd6"), 28);


	/* Check that we can write a series of different values onto the
	 * end of an existing buffer, without smashing what was already
	 * there.
	 */
	TEST_FEATURE ("with used buffer");
	ret = upstart_write_pack (&iovec, sizeof (buf), "ii", 98, 100);

	TEST_EQ (ret, 0);
	TEST_EQ (iovec.iov_len, 36);
	TEST_EQ_MEM (iovec.iov_base, ("\0\0\0\x64\x98\x76\x54\x32"
				      "\0\0\0\x0cstring value"
				      "\xff\xff\xff\xd6"
				      "\0\0\0\x62\0\0\0\x64"), 36);


	/* Check that -1 is returned if there's no enough space for the
	 * entire pack, and that the length is incremented to point past
	 * the available size.
	 */
	TEST_FEATURE ("with insufficient space");
	ret = upstart_write_pack (&iovec, sizeof (buf), "is", 19, "test");

	TEST_LT (ret, 0);
	TEST_EQ (iovec.iov_len, 48);
}

void
test_read_pack (void)
{
	struct iovec   iovec;
	char           buf[46];
	size_t         pos;
	char          *str;
	unsigned int   uint;
	int            ret, int1, int2;

	TEST_FUNCTION ("upstart_read_pack");
	iovec.iov_base = buf;
	iovec.iov_len = 46;
	memcpy (iovec.iov_base,("\0\0\0\x64\x98\x76\x54\x32"
				"\0\0\0\x0cstring value"
				"\xff\xff\xff\xd6"
				"\0\0\0\x62\0\0\0\x64"
				"\0\0\0\x13\0\0\0\x04te"), 46);
	pos = 0;

	/* Check that we can read a series of different values in a single
	 * function call, incrementing the pos variable past the lot.
	 */
	TEST_FEATURE ("with variables at start of buffer");
	ret = upstart_read_pack (&iovec, &pos, NULL, "iusi",
				 &int1, &uint, &str, &int2);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 28);
	TEST_EQ (int1, 100);
	TEST_EQ_U (uint, 0x98765432);
	TEST_ALLOC_SIZE (str, 13);
	TEST_EQ (str[12], '\0');
	TEST_EQ_STR (str, "string value");
	TEST_EQ (int2, -42);

	nih_free (str);


	/* Check that we can read a series of different values from a
	 * point already inside the buffer.  pos should be incremented,
	 * not set.
	 */
	TEST_FEATURE ("with variables inside buffer");
	ret = upstart_read_pack (&iovec, &pos, NULL, "ii",
				 &int1, &int2);

	TEST_EQ (ret, 0);
	TEST_EQ (pos, 36);
	TEST_EQ (int1, 98);
	TEST_EQ (int2, 100);


	/* Check that -1 is returned if there's not enough space in the
	 * buffer to the entire pack to exist, and that the length is
	 * incremented to point past the available size.
	 */
	TEST_FEATURE ("with insufficient space");
	str = NULL;
	ret = upstart_read_pack (&iovec, &pos, NULL, "is", &int1, &str);

	TEST_LT (ret, 0);
	TEST_EQ_P (str, NULL);
}



int
main (int   argc,
      char *argv[])
{
	test_write_int ();
	test_read_int ();
	test_write_unsigned ();
	test_read_unsigned ();
	test_write_string ();
	test_read_string ();
	test_write_header ();
	test_read_header ();
	test_write_pack ();
	test_read_pack ();

	return 0;
}