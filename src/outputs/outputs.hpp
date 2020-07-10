#ifndef OUTPUTS_OUTPUTS_HPP_
#define OUTPUTS_OUTPUTS_HPP_
//========================================================================================
// AthenaXXX astrophysical plasma code
// Copyright(C) 2020 James M. Stone <jmstone@ias.edu> and the Athena code team
// Licensed under the 3-clause BSD License (the "LICENSE")
//========================================================================================
//! \file outputs.hpp
//  \brief provides classes to handle ALL types of data output

#include <cstdio>  // std::size_t
#include <string>

#include "athena.hpp"
#include "athena_arrays.hpp"
#include "io_wrapper.hpp"

// forward declarations
class Mesh;
class ParameterInput;
class Coordinates;

//----------------------------------------------------------------------------------------
//! \struct OutputParameters
//  \brief  container for parameters read from <output> block in the input file

struct OutputParameters {
  int block_number;
  std::string block_name;
  std::string file_basename;
  std::string file_id;
  std::string variable;
  std::string file_type;
  std::string data_format;
  Real last_time, dt;
  int file_number;
  bool include_gzs;
  int islice, jslice, kslice;
  Real x1_slice, x2_slice, x3_slice;
};

//----------------------------------------------------------------------------------------
//! \struct OutputData
//  \brief container for output data and metadata; node in std::list

struct OutputData {
  std::string type;        // one of (SCALARS,VECTORS) used for vtk outputs
  std::string name;
  AthenaCenterArray<Real> data;  // array containing data (deep copy/slice)
};

//----------------------------------------------------------------------------------------
// \brief abstract base class for different output types (modes/formats); node in
//        std::list of OutputType created & stored in the Outputs class

class OutputType {
 public:
  explicit OutputType(OutputParameters oparams);
  virtual ~OutputType() = default;
  // copy constructor and assignment operator
  OutputType(const OutputType& copy_other) = default;
  OutputType& operator=(const OutputType& copy_other) = default;
  // move constructor and assignment operator
  OutputType(OutputType&&) = default;
  OutputType& operator=(OutputType&&) = default;

  // data
  OutputParameters output_params; // control data read from <output> block

  // functions
  // following pure virtual function must be implemented in all derived classes
//  virtual void WriteOutputFile(Mesh *pm, ParameterInput *pin, bool flag) = 0;

 protected:
};

//----------------------------------------------------------------------------------------
//! \class FormattedTableOutput
//  \brief derived OutputType class for formatted table (tabular) data

class FormattedTableOutput : public OutputType {
 public:
  explicit FormattedTableOutput(OutputParameters oparams) : OutputType(oparams) {}
//  void WriteOutputFile(Mesh *pm, ParameterInput *pin, bool flag) override;
};

//----------------------------------------------------------------------------------------
//! \class Outputs

//  \brief root class for all Athena++ outputs. Provides a singly linked list of
//  OutputTypes, with each node representing one mode/format of output to be made.

class Outputs {
 public:
  Outputs(std::unique_ptr<Mesh> &pm, std::unique_ptr<ParameterInput> &pin);
  ~Outputs();

 private:
  std::list<OutputType> output_list_;  // linked list of OutputTypes
};

#endif // OUTPUTS_OUTPUTS_HPP_
